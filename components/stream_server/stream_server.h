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

#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/uart/uart.h"

#include <memory>
#include <string>
#include <vector>

class StreamServerComponent : public esphome::Component {
public:
    StreamServerComponent() = default;
    explicit StreamServerComponent(esphome::uart::UARTComponent *stream) : stream_{stream} {}
    void set_uart_parent(esphome::uart::UARTComponent *parent) { this->stream_ = parent; }

    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

    void set_port(uint16_t port) { this->port_ = port; }
    void set_tcp_send_buffer_size(uint32_t bytes) { this->tcp_send_buffer_size_ = bytes; }
	int get_client_count() { return this->clients_.size(); }

    // Close every connected client socket. Used both by the framework's
    // on_shutdown() hook and by the OTA pre-flight in air-alarm-common, so
    // OTA traffic does not contend with high-throughput streaming.
    void disconnect_all();

    // Drive the configured UART's TX line LOW for `duration_ms` to generate
    // a break condition on the wire (ESP-IDF only). Asserts immediately and
    // schedules the restore via Component::set_timeout, so the call returns
    // promptly and the main loop is not parked for the break duration.
    // No-op on non-ESP-IDF builds.
    void send_break(uint32_t duration_ms);

protected:
    void accept();
    void cleanup();
    void read();
    void write();

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
    };

    esphome::uart::UARTComponent *stream_{nullptr};
    std::unique_ptr<esphome::socket::Socket> socket_{};
    uint16_t port_{6638};
    // 0 = use the system default (CONFIG_LWIP_TCP_SND_BUF_DEFAULT). When
    // non-zero, accepted client sockets get setsockopt(SO_SNDBUF, ...)
    // so this port can absorb larger bursts without affecting other
    // sockets on the device. Requires CONFIG_LWIP_SO_SNDBUF=y.
    uint32_t tcp_send_buffer_size_{0};
    std::vector<Client> clients_{};

    // Overflow tracking. RX backlog warns when the UART RX buffer is filling
    // faster than we drain (rate-limited 1/s); tcp write loss aggregates short
    // / failed socket writes and reports the byte total every 5s.
    uint32_t rx_backlog_last_log_ms_{0};
    uint32_t tcp_write_lost_bytes_{0};
    uint32_t tcp_write_last_log_ms_{0};

    // Lifetime counters since boot — kept ticking so a future caller can
    // query them, but not periodically logged (the per-event WARNs are
    // sufficient and the periodic line was log noise on quiet ports).
    uint32_t accepted_total_{0};
    uint64_t bytes_rx_total_{0};       // UART -> TCP direction
    uint64_t bytes_tx_total_{0};       // TCP -> UART direction
    uint64_t bytes_lost_total_{0};     // cumulative TCP write loss
    uint32_t accept_fail_last_log_ms_{0};
};

// stream_server.send_break: <id>, duration: <time>
// Wraps StreamServerComponent::send_break() so it is invocable from any
// ESPHome automation. duration is captured at codegen time as a fixed
// uint32_t millisecond value (not templatable) — keeps the action shape
// minimal and matches typical break-generation use cases.
template<typename... Ts>
class StreamServerSendBreakAction : public esphome::Action<Ts...>,
                                    public esphome::Parented<StreamServerComponent> {
 public:
    void set_duration(uint32_t duration_ms) { this->duration_ms_ = duration_ms; }
    void play(Ts... x) override {
        this->parent_->send_break(this->duration_ms_);
    }

 protected:
    uint32_t duration_ms_{0};
};
