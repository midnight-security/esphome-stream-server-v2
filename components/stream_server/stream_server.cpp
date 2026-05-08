/* Copyright (C) 2020-2022 Oxan van Leeuwen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

// TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT live in lwip/tcp.h on ESP-IDF.
// errno is needed for dead-socket detection in read().
#include <errno.h>
#include <lwip/sockets.h>

#ifdef USE_OTA_STATE_CALLBACK
#include "esphome/components/ota/ota_backend.h"
#endif

// IDFUARTComponent::get_hw_serial_number() exposes the underlying ESP-IDF
// uart_port_t, which we need for uart_set_line_inverse() / uart_wait_tx_done().
#ifdef USE_ESP_IDF
#include "esphome/components/uart/uart_component_esp_idf.h"
#endif

static const char *TAG = "streamserver";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    struct sockaddr_in bind_addr = {
        .sin_len = sizeof(struct sockaddr_in),
        .sin_family = AF_INET,
        .sin_port = htons(this->port_),
        .sin_addr = {
            .s_addr = ESPHOME_INADDR_ANY,
        }
    };

    this->socket_ = socket::socket(AF_INET, SOCK_STREAM, PF_INET);
	
    struct timeval timeout;      
    timeout.tv_sec = 0;
    timeout.tv_usec = 20000; // ESPHome recommends 20-30 ms max for timeouts
    
    this->socket_->setsockopt(SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    this->socket_->setsockopt(SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
  
    int bind_rc = this->socket_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr),
                                       sizeof(struct sockaddr_in));
    if (bind_rc < 0)
        ESP_LOGE(TAG, "Port %u: bind() failed errno=%d", this->port_, errno);
    int listen_rc = this->socket_->listen(8);
    if (listen_rc < 0)
        ESP_LOGE(TAG, "Port %u: listen() failed errno=%d", this->port_, errno);
    else
        ESP_LOGCONFIG(TAG, "Port %u: listening", this->port_);

#ifdef USE_OTA_STATE_CALLBACK
    // Drop streaming clients as soon as ANY OTA platform begins so the
    // high-throughput UART→TCP path stops contending with the OTA transfer
    // for WiFi airtime and core-1 CPU. Fires once per OTA attempt.
    ota::get_global_ota_callback()->add_on_state_callback(
        [this](ota::OTAState state, float, uint8_t, ota::OTAComponent *) {
            if (state != ota::OTA_STARTED)
                return;
            if (this->clients_.empty())
                return;
            ESP_LOGW(TAG, "OTA started — disconnecting %d stream client(s) on port %u",
                     (int) this->clients_.size(), this->port_);
            this->disconnect_all();
        });
#endif
}

void StreamServerComponent::loop() {
    this->accept();
    this->read();
    this->write();
    this->cleanup();
}

void StreamServerComponent::accept() {
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(struct sockaddr_in);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket) {
        // accept() returns nullptr both for "no pending connection"
        // (errno EAGAIN/EWOULDBLOCK, expected on most loop iterations) and
        // for real errors (e.g. ECONNABORTED if a peer SYN'd then RST'd
        // before we accepted, or EMFILE if we're out of FDs). Rate-limit
        // the real-error log so a stuck accept loop cannot flood.
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            uint32_t now = millis();
            if ((now - this->accept_fail_last_log_ms_) >= 1000) {
                ESP_LOGW(TAG, "Port %u: accept() failed errno=%d",
                         this->port_, errno);
                this->accept_fail_last_log_ms_ = now;
            }
        }
        return;
    }

    socket->setblocking(false);
    this->accepted_total_++;

    // TCP keepalive — detect a peer that vanished during quiet periods.
    // Without keepalive, lwIP only notices a dead peer when it tries to send
    // and gets no ACK, which can take minutes; meanwhile our writes succeed
    // (into the local send buffer) and bytes are silently dropped. With these
    // settings lwIP probes after 30s idle, every 5s, dropping after 3 misses
    // — peer death detected within ~45s of going quiet.
    int yes = 1;
    int idle = 30, intvl = 5, cnt = 3;
    socket->setsockopt(SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    socket->setsockopt(IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    socket->setsockopt(IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    socket->setsockopt(IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

    std::string identifier = inet_ntoa(client_addr.sin_addr);
    this->clients_.emplace_back(std::move(socket), identifier);
    // WARN level so the lifecycle event reaches MQTT via the consumer's
    // logger.on_message WARN+ forwarder. Once-per-client event — no
    // log-spam risk.
    ESP_LOGW(TAG, "Port %u: client connected from %s (%d total)",
             this->port_, identifier.c_str(), (int) this->clients_.size());
}

void StreamServerComponent::cleanup() {
    auto discriminator = [](const Client &client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    this->clients_.erase(last_client, this->clients_.end());
}

void StreamServerComponent::read() {
    // Threshold is 50% of the data UART's rx_buffer_size (8192). Anything
    // above this means the drain loop is falling behind and bytes are at
    // imminent risk of being dropped by the ESP-IDF UART driver.
    static const int RX_BACKLOG_WARN = 4096;

    uint32_t now = millis();
    int initial_avail = this->stream_->available();
    if (initial_avail >= RX_BACKLOG_WARN &&
        (now - this->rx_backlog_last_log_ms_) >= 1000) {
        ESP_LOGW(TAG, "Port %u: UART RX backlog %d bytes — drain falling behind",
                 this->port_, initial_avail);
        this->rx_backlog_last_log_ms_ = now;
    }

    int len;
    // 1460 ≈ one TCP MSS over Ethernet — pack each socket->write() into a
    // single TCP segment. Bigger than this just gets fragmented by lwIP;
    // smaller costs syscall overhead. At 921600 baud and 8KB rx_buffer,
    // 1460-byte chunks drain in ~6 iterations vs. ~64 with the old 128.
    while ((len = this->stream_->available()) > 0) {
        char buf[1460];
        len = std::min(len, (int) sizeof(buf));
        this->stream_->read_array(reinterpret_cast<uint8_t*>(buf), len);
        this->bytes_rx_total_ += (uint64_t) len;
        for (Client &client : this->clients_) {
            if (client.disconnected)
                continue;
            ssize_t written = client.socket->write(buf, len);
            if (written != len) {
                // Short or failed write — bytes are dropped on this client's
                // TCP path. Aggregate so a flood of losses does not flood logs.
                int lost = (written < 0) ? len : (len - (int) written);
                this->tcp_write_lost_bytes_ += lost;
                this->bytes_lost_total_ += (uint64_t) lost;
                // Distinguish transient back-pressure (EAGAIN/EWOULDBLOCK)
                // from a dead peer (EPIPE / ECONNRESET / ENOTCONN / EBADF).
                // Without this, a closed client socket would never be marked
                // disconnected and we would write into the void forever.
                if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Port %u: write to %s failed (errno=%d) — marking dead",
                             this->port_, client.identifier.c_str(), errno);
                    client.disconnected = true;
                }
            }
        }
    }

    // Periodic flush of accumulated TCP write loss
    if (this->tcp_write_lost_bytes_ > 0 &&
        (now - this->tcp_write_last_log_ms_) >= 5000) {
        ESP_LOGW(TAG, "Port %u: TCP write loss %u bytes in last 5s",
                 this->port_, this->tcp_write_lost_bytes_);
        this->tcp_write_lost_bytes_ = 0;
        this->tcp_write_last_log_ms_ = now;
    }
}

void StreamServerComponent::write() {
    // Symmetric with read() — one MSS per syscall.
    uint8_t buf[1460];
    ssize_t len;
    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;
        while ((len = client.socket->read(&buf, sizeof(buf))) > 0){
            this->stream_->write_array(buf, len);
            this->bytes_tx_total_ += (uint64_t) len;
		}
        if (len == 0) {
            // Clean FIN from peer. WARN level for MQTT visibility, same
            // rationale as the connect log in accept().
            ESP_LOGW(TAG, "Port %u: client %s disconnected (%d remaining)",
                     this->port_, client.identifier.c_str(),
                     (int) this->clients_.size() - 1);
            client.disconnected = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // Non-transient read error — peer died. Surface at WARN so the
            // MQTT log forwarder picks it up, and mark the client dead so
            // cleanup() removes it. Symmetric to the write-loss handling
            // in read().
            ESP_LOGW(TAG, "Port %u: read from %s failed (errno=%d) — marking dead",
                     this->port_, client.identifier.c_str(), errno);
            client.disconnected = true;
        }
    }
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
    std::string ip_str = "";
    for (auto &ip : network::get_ip_addresses()) {
      if (ip.is_set()) {
        ip_str += " " + ip.str();
      }
    }
    ESP_LOGCONFIG(TAG, "  Address:%s", ip_str.c_str());
    ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
}

void StreamServerComponent::disconnect_all() {
    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;
        client.socket->shutdown(SHUT_RDWR);
        client.disconnected = true;   // cleanup() removes on next loop()
    }
}

void StreamServerComponent::clear_buffers() {
    int n_clients = (int) this->clients_.size();
    ESP_LOGW(TAG, "Port %u: clear_buffers — disconnecting %d client(s) and flushing UART RX",
             this->port_, n_clients);

    // Drop every connected client. lwIP discards each socket's queued send
    // buffer on close, so any pre-call bytes already enqueued for transmit
    // never reach the host. The host will reconnect and see only data
    // produced after this call.
    this->disconnect_all();

#ifdef USE_ESP_IDF
    // Flush the ESP-IDF UART RX buffer — anything the peer sent before the
    // call (including residual data from a pre-reset peer state) is
    // discarded so it does not get forwarded to the next client.
    auto *idf = static_cast<uart::IDFUARTComponent *>(this->stream_);
    uart_port_t uart_num = (uart_port_t) idf->get_hw_serial_number();
    uart_flush_input(uart_num);
#endif
}

void StreamServerComponent::send_break(uint32_t duration_ms) {
#ifdef USE_ESP_IDF
    auto *idf = static_cast<uart::IDFUARTComponent *>(this->stream_);
    uart_port_t uart_num = (uart_port_t) idf->get_hw_serial_number();
    uint16_t port = this->port_;

    // WARN level so these events propagate to MQTT log via the consumer's
    // logger.on_message forward (e.g. air-alarm-common's WARN+ filter).
    // send_break is intentionally rare — bumping these does not risk log spam.
    ESP_LOGW(TAG, "Port %u: send_break asserting LOW on UART_NUM_%d for %ums",
             port, (int) uart_num, (unsigned) duration_ms);

    // Drain in-flight TX so the break starts cleanly. 50ms upper bound — an
    // already-idle TX returns immediately.
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(50));

    // Invert TX → idle-HIGH becomes LOW = continuous break condition until
    // the inversion is disabled.
    uart_set_line_inverse(uart_num, UART_SIGNAL_TXD_INV);

    // Schedule restore via the Component scheduler so the main loop is not
    // parked for the break duration. Capture by value (uart_num + port) —
    // the lambda may outlive the immediate call frame.
    this->set_timeout("send_break_restore", duration_ms, [uart_num, port]() {
        uart_set_line_inverse(uart_num, UART_SIGNAL_INV_DISABLE);
        ESP_LOGW(TAG, "Port %u: send_break restored UART_NUM_%d", port, (int) uart_num);
    });
#else
    ESP_LOGW(TAG, "Port %u: send_break is only implemented on ESP-IDF (no-op)",
             this->port_);
#endif
}

void StreamServerComponent::on_shutdown() {
    this->disconnect_all();
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier)
    : socket(std::move(socket)), identifier{identifier}
{
}
