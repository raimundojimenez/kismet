/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "eventbus.h"

event_bus::event_bus() {
    mutex.set_name("event_bus");
    handler_mutex.set_name("event_bus_handler");

    next_cbl_id = 1;

    shutdown = false;

    eventbus_event_id = 
        Globalreg::globalreg->entrytracker->register_field("kismet.eventbus.event",
                tracker_element_factory<eventbus_event>(),
                "Eventbus event");

    event_cl.lock();

    event_dispatch_t =
        std::thread([this]() {
                thread_set_process_name("eventbus");
                event_queue_dispatcher();
            });

}

event_bus::~event_bus() {
    shutdown = true;

    event_cl.unlock(0);
    event_dispatch_t.join();
}

std::shared_ptr<eventbus_event> event_bus::get_eventbus_event(const std::string& event_type) {
    return std::make_shared<eventbus_event>(eventbus_event_id, event_type);
}

void event_bus::event_queue_dispatcher() {
    local_demand_locker l(&mutex);

    while (!shutdown && 
            !Globalreg::globalreg->spindown && 
            !Globalreg::globalreg->fatal_condition &&
            !Globalreg::globalreg->complete) {
        // Lock while we examine the queue
        l.lock();

        if (event_queue.size() > 0) {
            auto e = event_queue.front();
            event_queue.pop();

            auto ch_listeners = callback_table.find(e->get_event_id());
			auto ch_all_listeners = callback_table.find("*");

            if (ch_listeners == callback_table.end() && ch_all_listeners == callback_table.end()) {
                l.unlock();
                continue;
            }

            // Lock the handler mutex while we're processing an event
            {
                local_locker rl(&handler_mutex);

                // Unlock the rest of the eventbus
                l.unlock();

                if (ch_listeners != callback_table.end()) {
                    for (const auto& cbl : ch_listeners->second) {
                        cbl->cb(e);
                    }
                }

                if (ch_all_listeners != callback_table.end()) {
                    for (const auto& cbl : ch_all_listeners->second) {
                        cbl->cb(e);
                    }
                }

            }

            // Loop for more events
            continue;
        }

        // Reset the lock
        event_cl.lock();
      
        // Unlock our hold on the system
        l.unlock();

        // Wait until new events
        event_cl.block_until();
    }
}

unsigned long event_bus::register_listener(const std::string& channel, cb_func cb) {
    local_locker l(&handler_mutex);

    auto cbl = std::make_shared<callback_listener>(std::list<std::string>{channel}, cb, next_cbl_id++);

    callback_table[channel].push_back(cbl);
    callback_id_table[cbl->id] = cbl;

    return cbl->id;
}

unsigned long event_bus::register_listener(const std::list<std::string>& channels, cb_func cb) {
    local_locker l(&handler_mutex);

    auto cbl = std::make_shared<callback_listener>(channels, cb, next_cbl_id++);

    for (auto i : channels) {
        callback_table[i].push_back(cbl);
    }

    callback_id_table[cbl->id] = cbl;

    return cbl->id;
}

void event_bus::remove_listener(unsigned long id) {
    local_locker l(&handler_mutex);

    // Find matching cbl
    auto cbl = callback_id_table.find(id);
    if (cbl == callback_id_table.end())
        return;

    // Match all channels this cbl is subscribed to
    for (auto c : cbl->second->channels) {
        auto cb_list = callback_table[c];

        // remove from each channel
        for (auto cbi = cb_list.begin(); cbi != cb_list.end(); ++cbi) {
            if ((*cbi)->id == id) {
                cb_list.erase(cbi);
                break;
            }
        }
    }

    // Remove from CBL ID table
    callback_id_table.erase(cbl);
}

