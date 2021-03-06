/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/network/sessions/session_inbound.hpp>

#include <cstddef>
#include <functional>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/network/p2p.hpp>
#include <bitcoin/network/protocols/protocol_address_31402.hpp>
#include <bitcoin/network/protocols/protocol_ping_31402.hpp>
#include <bitcoin/network/protocols/protocol_ping_60001.hpp>

namespace libbitcoin {
namespace network {

#define CLASS session_inbound

using namespace std::placeholders;

session_inbound::session_inbound(p2p& network, bool notify_on_connect)
  : session(network, notify_on_connect),
    connection_limit_(settings_.inbound_connections +
        settings_.outbound_connections),
    CONSTRUCT_TRACK(session_inbound)
{
}

// Start sequence.
// ----------------------------------------------------------------------------

void session_inbound::start(result_handler handler)
{
    if (settings_.inbound_port == 0 || settings_.inbound_connections == 0)
    {
        LOG_INFO(LOG_NETWORK)
            << "Not configured for accepting incoming connections.";
        handler(error::success);
        return;
    }

    session::start(CONCURRENT2(handle_started, _1, handler));
}

void session_inbound::handle_started(const code& ec, result_handler handler)
{
    if (ec)
    {
        handler(ec);
        return;
    }

    acceptor_ = create_acceptor();

    // Relay stop to the acceptor.
    subscribe_stop(BIND1(handle_stop, _1));

    // START LISTENING ON PORT
    const auto error_code = acceptor_->listen(settings_.inbound_port);

    if (error_code)
    {
        LOG_ERROR(LOG_NETWORK)
            << "Error starting listener: " << ec.message();
        handler(error_code);
        return;
    }

    start_accept();

    // This is the end of the start sequence.
    handler(error::success);
}

void session_inbound::handle_stop(const code& ec)
{
    // Signal the stop of listener/accept attempt.
    acceptor_->stop(ec);
}

// Accept sequence.
// ----------------------------------------------------------------------------

void session_inbound::start_accept()
{
    if (stopped())
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Suspended inbound connection.";
        return;
    }

    // ACCEPT THE NEXT INCOMING CONNECTION
    acceptor_->accept(BIND2(handle_accept, _1, _2));
}

void session_inbound::handle_accept(const code& ec, channel::ptr channel)
{
    if (stopped(ec))
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Suspended inbound connection.";
        return;
    }

    // Start accepting again immediately, regardless of previous error.
    start_accept();

    if (ec)
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Failure accepting connection: " << ec.message();
        return;
    }

    if (blacklisted(channel->authority()))
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Rejected inbound connection from ["
            << channel->authority() << "] due to blacklisted address.";
        return;
    }

    if (connection_count() >= connection_limit_)
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Rejected inbound connection from ["
            << channel->authority() << "] due to connection limit.";
        return;
    }
   
    LOG_INFO(LOG_NETWORK)
        << "Connected inbound channel [" << channel->authority() << "]";

    register_channel(channel, 
        BIND2(handle_channel_start, _1, channel),
        BIND1(handle_channel_stop, _1));
}

void session_inbound::handle_channel_start(const code& ec,
    channel::ptr channel)
{
    if (ec)
    {
        LOG_INFO(LOG_NETWORK)
            << "Inbound channel failed to start [" << channel->authority()
            << "] " << ec.message();
        return;
    }

    attach_protocols(channel);
};

void session_inbound::attach_protocols(channel::ptr channel)
{
    if (channel->negotiated_version() >= message::version::level::bip31)
        attach<protocol_ping_60001>(channel)->start();
    else
        attach<protocol_ping_31402>(channel)->start();

    attach<protocol_address_31402>(channel)->start();
}

void session_inbound::handle_channel_stop(const code& ec)
{
    LOG_DEBUG(LOG_NETWORK)
        << "Inbound channel stopped: " << ec.message();
}

// Channel start sequence.
// ----------------------------------------------------------------------------
// Check pending outbound connections for loopback to this inbound.

void session_inbound::handshake_complete(channel::ptr channel,
    result_handler handle_started)
{    
    if (pending(channel->peer_version()->nonce()))
    {
        LOG_DEBUG(LOG_NETWORK)
            << "Rejected connection from [" << channel->authority()
            << "] as loopback.";
        handle_started(error::accept_failed);
        return;
    }

    session::handshake_complete(channel, handle_started);
}

} // namespace network
} // namespace libbitcoin
