# Copyright (C) 2021-2022 Oxan van Leeuwen
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import uart
from esphome.const import CONF_DURATION, CONF_ID, CONF_PORT
from esphome.util import parse_esphome_version

# ESPHome doesn't know the Stream abstraction yet, so hardcode to use a UART for now.

AUTO_LOAD = ["socket", "binary_sensor"]

DEPENDENCIES = ["uart", "network"]

MULTI_CONF = True

ns = cg.global_ns
StreamServerComponent = ns.class_("StreamServerComponent", cg.Component)
SendBreakAction = ns.class_("StreamServerSendBreakAction", automation.Action)

CONF_TCP_SEND_BUFFER_SIZE = "tcp_send_buffer_size"

CONFIG_SCHEMA = (
	cv.Schema(
		{
			cv.GenerateID(): cv.declare_id(StreamServerComponent),
			cv.Optional(CONF_PORT): cv.port,
			# Per-port override of the lwIP TCP send buffer cap. 0 / unset
			# falls back to CONFIG_LWIP_TCP_SND_BUF_DEFAULT. Max is 65535
			# without window scaling (16-bit TCP window field). Requires
			# CONFIG_LWIP_SO_SNDBUF=y in sdkconfig (off by default in
			# ESP-IDF).
			cv.Optional(CONF_TCP_SEND_BUFFER_SIZE): cv.int_range(
				min=2048, max=65535
			),
		}
	)
		.extend(cv.COMPONENT_SCHEMA)
		.extend(uart.UART_DEVICE_SCHEMA)
)

def to_code(config):
	var = cg.new_Pvariable(config[CONF_ID])
	if CONF_PORT in config:
		cg.add(var.set_port(config[CONF_PORT]))
	if CONF_TCP_SEND_BUFFER_SIZE in config:
		cg.add(var.set_tcp_send_buffer_size(config[CONF_TCP_SEND_BUFFER_SIZE]))

	yield cg.register_component(var, config)
	yield uart.register_uart_device(var, config)


# stream_server.send_break: <stream_server_id>, duration: <time>
# Asserts a UART break (TX line LOW) for the given duration. The action
# returns immediately; restore is scheduled via Component::set_timeout.
SEND_BREAK_SCHEMA = cv.Schema({
	cv.GenerateID(): cv.use_id(StreamServerComponent),
	cv.Required(CONF_DURATION): cv.positive_time_period_milliseconds,
})


@automation.register_action("stream_server.send_break", SendBreakAction, SEND_BREAK_SCHEMA)
def send_break_to_code(config, action_id, template_arg, args):
	paren = yield cg.get_variable(config[CONF_ID])
	var = cg.new_Pvariable(action_id, template_arg)
	yield cg.register_parented(var, paren)
	cg.add(var.set_duration(config[CONF_DURATION].total_milliseconds))
	yield var

esphome_version = parse_esphome_version()

	# Request UART to wake the main loop when data arrives for low-latency processing
# Apply the fix only for versions 2025.12.x through 2026.2.x
if (2025, 12, 0) <= esphome_version < (2026, 3, 0):
    uart.request_wake_loop_on_rx()
