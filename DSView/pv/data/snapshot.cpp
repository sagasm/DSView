/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include "snapshot.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
 
namespace pv {
namespace data {

Snapshot::Snapshot(int unit_size, uint64_t total_sample_count, unsigned int channel_num) :
    _data(NULL),
    _capacity(0),
    _channel_num(channel_num),
    _sample_count(0),
    _total_sample_count(total_sample_count),
    _ring_sample_count(0),
    _unit_size(unit_size),
    _memory_failed(false),
    _last_ended(true)
{
    assert(_unit_size > 0);
    _unit_bytes = 1;
    _unit_pitch = 0;
    _have_data = false;
}

Snapshot::~Snapshot()
{
    free_data();
}

void Snapshot::free_data()
{
    if (_data) {
        free(_data);
        _data = NULL;
        _capacity = 0;
        _sample_count = 0;
    }
    _ch_index.clear();
}

bool Snapshot::empty()
{
    if (get_sample_count() == 0)
        return true;
    else
        return false;
}

uint64_t Snapshot::get_sample_count()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _sample_count;
}
 
uint64_t Snapshot::get_ring_start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return ring_start();    
}

uint64_t Snapshot::get_ring_end()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return ring_end();     
}
 
uint64_t Snapshot::ring_start()
{ 
    if (_sample_count < _total_sample_count)
        return 0;
    else
        return _ring_sample_count;
}
 
uint64_t Snapshot::ring_end()
{ 
    if (_sample_count == 0)
        return 0;
    else if (_ring_sample_count == 0)
        return _total_sample_count - 1;
    else
        return _ring_sample_count - 1;
}

void Snapshot::capture_ended()
{
    set_last_ended(true);
}

} // namespace data
} // namespace pv
