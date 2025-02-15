/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
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

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
 
#include "dsosnapshot.h"
#include "../dsvdef.h"

using namespace std;

namespace pv {
namespace data {

const int DsoSnapshot::EnvelopeScalePower = 8;
const int DsoSnapshot::EnvelopeScaleFactor = 1 << EnvelopeScalePower;
const float DsoSnapshot::LogEnvelopeScaleFactor =
	logf(EnvelopeScaleFactor);
const uint64_t DsoSnapshot::EnvelopeDataUnit = 4*1024;	// bytes

const int DsoSnapshot::VrmsScaleFactor = 1 << 8;

DsoSnapshot::DsoSnapshot() :
    Snapshot(sizeof(uint16_t), 1, 1),
    _envelope_en(false),
    _envelope_done(false),
    _instant(false)
{
	memset(_envelope_levels, 0, sizeof(_envelope_levels));
}

DsoSnapshot::~DsoSnapshot()
{
    free_envelop();
}

void DsoSnapshot::free_envelop()
{
    for (unsigned int i = 0; i < _channel_num; i++) {
        for(auto &e : _envelope_levels[i]) {
            if (e.samples)
                free(e.samples);
        }
    }
    memset(_envelope_levels, 0, sizeof(_envelope_levels));
}

void DsoSnapshot::init()
{
    std::lock_guard<std::mutex> lock(_mutex);
    init_all();    
}

void DsoSnapshot::init_all()
{
    _sample_count = 0;
    _ring_sample_count = 0;
    _memory_failed = false;
    _last_ended = true;
    _envelope_done = false;
    _ch_enable.clear();

    for (unsigned int i = 0; i < _channel_num; i++) {
        for (unsigned int level = 0; level < ScaleStepCount; level++) {
            _envelope_levels[i][level].length = 0;
            _envelope_levels[i][level].data_length = 0;
        }
    }
}

void DsoSnapshot::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    free_data();
    free_envelop();
    init_all();
    _have_data  = false;
}

void DsoSnapshot::first_payload(const sr_datafeed_dso &dso, uint64_t total_sample_count,
                                std::map<int, bool> ch_enable, bool instant)
{
    bool re_alloc = false;
    unsigned int channel_num = 0;
    for (auto& iter:ch_enable) {
        if (iter.second)
            channel_num++;
    }
    assert(channel_num != 0);

    if (total_sample_count != _total_sample_count ||
        channel_num != _channel_num)
        re_alloc = true;

    _total_sample_count = total_sample_count;
    _channel_num = channel_num;
    _instant = instant;
    _ch_enable = ch_enable;

    bool isOk = true;
    uint64_t size = _total_sample_count * _channel_num + sizeof(uint64_t);
    if (re_alloc || size != _capacity) {
        free_data();
        _data = malloc(size);
        if (_data) {
            free_envelop();
            for (unsigned int i = 0; i < _channel_num; i++) {
                uint64_t envelop_count = _total_sample_count / EnvelopeScaleFactor;
                for (unsigned int level = 0; level < ScaleStepCount; level++) {
                    envelop_count = ((envelop_count + EnvelopeDataUnit - 1) /
                            EnvelopeDataUnit) * EnvelopeDataUnit;
                    _envelope_levels[i][level].samples = (EnvelopeSample*)malloc(envelop_count * sizeof(EnvelopeSample));
                    if (!_envelope_levels[i][level].samples) {
                        isOk = false;
                        break;
                    }
                    envelop_count = envelop_count / EnvelopeScaleFactor;
                }
                if (!isOk)
                    break;
            }
        } else {
            isOk = true;
        }
    }

    if (isOk) {
        _capacity = size;
        _memory_failed = false;
        append_payload(dso);
        _last_ended = false;
    } else {
        free_data();
        free_envelop();
        _memory_failed = true;
    }
}

void DsoSnapshot::append_payload(const sr_datafeed_dso &dso)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_channel_num > 0 && dso.num_samples != 0) {
        append_data(dso.data, dso.num_samples, _instant);

        // Generate the first mip-map from the data
        if (_envelope_en)
            append_payload_to_envelope_levels(dso.samplerate_tog);

        _have_data = true;
    }
}

void DsoSnapshot::append_data(void *data, uint64_t samples, bool instant)
{
    if (instant) {
        if(_sample_count + samples > _total_sample_count)
            samples = _total_sample_count - _sample_count;
        memcpy((uint8_t*)_data + _sample_count * _channel_num, data, samples*_channel_num);
        _sample_count += samples;
    } else {
        memcpy((uint8_t*)_data, data, samples*_channel_num);
        _sample_count = samples;
    }

}

void DsoSnapshot::enable_envelope(bool enable)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_envelope_done && enable)
        append_payload_to_envelope_levels(true);
    _envelope_en = enable;
}

const uint8_t *DsoSnapshot::get_samples(
    int64_t start_sample, int64_t end_sample, uint16_t index)
{
   std::lock_guard<std::mutex> lock(_mutex);
    (void)end_sample;

	assert(start_sample >= 0);
    assert(start_sample < (int64_t)sample_count());
	assert(end_sample >= 0);
    assert(end_sample < (int64_t)sample_count());
	assert(start_sample <= end_sample);


    return (uint8_t*)_data + start_sample * _channel_num + index * (_channel_num != 1);
}

void DsoSnapshot::get_envelope_section(EnvelopeSection &s,
    uint64_t start, uint64_t end, float min_length, int probe_index)
{
	assert(end <= get_sample_count());
	assert(start <= end);
	assert(min_length > 0);

    if (!_envelope_done) {
        s.length = 0;
        return;
    }

	const unsigned int min_level = max((int)floorf(logf(min_length) /
		LogEnvelopeScaleFactor) - 1, 0);
	const unsigned int scale_power = (min_level + 1) *
		EnvelopeScalePower;
	start >>= scale_power;
	end >>= scale_power;

	s.start = start << scale_power;
	s.scale = 1 << scale_power;
    if (_envelope_levels[probe_index][min_level].length == 0)
        s.length = 0;
    else
        s.length = end - start;
//	s.samples = new EnvelopeSample[s.length];
//	memcpy(s.samples, _envelope_levels[min_level].samples + start,
//		s.length * sizeof(EnvelopeSample));
    s.samples = _envelope_levels[probe_index][min_level].samples + start;
}

void DsoSnapshot::reallocate_envelope(Envelope &e)
{
	const uint64_t new_data_length = ((e.length + EnvelopeDataUnit - 1) /
		EnvelopeDataUnit) * EnvelopeDataUnit;
    if (new_data_length > e.data_length)
	{
		e.data_length = new_data_length;
//		e.samples = (EnvelopeSample*)realloc(e.samples,
//			new_data_length * sizeof(EnvelopeSample));
	}
}

void DsoSnapshot::append_payload_to_envelope_levels(bool header)
{
    for (unsigned int i = 0; i < _channel_num; i++) {
        Envelope &e0 = _envelope_levels[i][0];
        uint64_t prev_length;
        EnvelopeSample *dest_ptr;

        if (header)
            prev_length = 0;
        else
            prev_length = e0.length;
        e0.length = _sample_count / EnvelopeScaleFactor;

        if (e0.length == 0)
            return;
        if (e0.length == prev_length)
            prev_length = 0;

        // Expand the data buffer to fit the new samples
        reallocate_envelope(e0);

        dest_ptr = e0.samples + prev_length;

        // Iterate through the samples to populate the first level mipmap
        const uint8_t *const stop_src_ptr = (uint8_t*)_data +
            e0.length * EnvelopeScaleFactor * _channel_num;
        for (const uint8_t *src_ptr = (uint8_t*)_data +
            prev_length * EnvelopeScaleFactor * _channel_num + i;
            src_ptr < stop_src_ptr; src_ptr += EnvelopeScaleFactor * _channel_num)
        {
            const uint8_t * begin_src_ptr =
                src_ptr;
            const uint8_t *const end_src_ptr =
                src_ptr + EnvelopeScaleFactor * _channel_num;

            EnvelopeSample sub_sample;
            sub_sample.min = *begin_src_ptr;
            sub_sample.max = *begin_src_ptr;
            //begin_src_ptr += _channel_num;
            while (begin_src_ptr < end_src_ptr)
            {
                sub_sample.min = min(sub_sample.min, *begin_src_ptr);
                sub_sample.max = max(sub_sample.max, *begin_src_ptr);
                begin_src_ptr += _channel_num;
            }
            *dest_ptr++ = sub_sample;
        }

        // Compute higher level mipmaps
        for (unsigned int level = 1; level < ScaleStepCount; level++)
        {
            Envelope &e = _envelope_levels[i][level];
            const Envelope &el = _envelope_levels[i][level-1];

            // Expand the data buffer to fit the new samples
            if (header)
                prev_length = 0;
            else
                prev_length = e.length;
            e.length = el.length / EnvelopeScaleFactor;

            // Break off if there are no more samples to computed
    //		if (e.length == prev_length)
    //			break;
            if (e.length == 0)
                break;
            if (e.length == prev_length)
                prev_length = 0;

            reallocate_envelope(e);

            // Subsample the level lower level
            const EnvelopeSample *src_ptr =
                el.samples + prev_length * EnvelopeScaleFactor;
            const EnvelopeSample *const end_dest_ptr = e.samples + e.length;
            for (dest_ptr = e.samples + prev_length;
                dest_ptr < end_dest_ptr; dest_ptr++)
            {
                const EnvelopeSample *const end_src_ptr =
                    src_ptr + EnvelopeScaleFactor;

                EnvelopeSample sub_sample = *src_ptr++;
                while (src_ptr < end_src_ptr)
                {
                    sub_sample.min = min(sub_sample.min, src_ptr->min);
                    sub_sample.max = max(sub_sample.max, src_ptr->max);
                    src_ptr++;
                }

                *dest_ptr = sub_sample;
            }
        }
    }
    _envelope_done = true;
}

double DsoSnapshot::cal_vrms(double zero_off, int index)
{
    assert(index >= 0);
    //assert(index < _channel_num);

    // root-meam-squart value
    double vrms_pre = 0;
    double vrms = 0;
    double tmp;

    // Iterate through the samples to populate the first level mipmap
    const uint8_t *const stop_src_ptr = (uint8_t*)_data +
        get_sample_count() * _channel_num;
    for (const uint8_t *src_ptr = (uint8_t*)_data + (index % _channel_num);
        src_ptr < stop_src_ptr; src_ptr += VrmsScaleFactor * _channel_num)
    {
        const uint8_t * begin_src_ptr =
            src_ptr;
        const uint8_t *const end_src_ptr =
            src_ptr + VrmsScaleFactor * _channel_num;

        while (begin_src_ptr < end_src_ptr)
        {
            tmp = (zero_off - *begin_src_ptr);
            vrms += tmp * tmp;
            begin_src_ptr += _channel_num;
        }
        vrms = vrms_pre + vrms / get_sample_count();
        vrms_pre = vrms;
    }
    vrms = pow(vrms, 0.5);

    return vrms;
}

double DsoSnapshot::cal_vmean(int index)
{
    assert(index >= 0);
    //assert(index < _channel_num);

    // mean value
    double vmean_pre = 0;
    double vmean = 0;

    // Iterate through the samples to populate the first level mipmap
    const uint8_t *const stop_src_ptr = (uint8_t*)_data +
        get_sample_count() * _channel_num;
    for (const uint8_t *src_ptr = (uint8_t*)_data + (index % _channel_num);
        src_ptr < stop_src_ptr; src_ptr += VrmsScaleFactor * _channel_num)
    {
        const uint8_t * begin_src_ptr =
            src_ptr;
        const uint8_t *const end_src_ptr =
            src_ptr + VrmsScaleFactor * _channel_num;

        while (begin_src_ptr < end_src_ptr)
        {
            vmean += *begin_src_ptr;
            begin_src_ptr += _channel_num;
        }
        vmean = vmean_pre + vmean / get_sample_count();
        vmean_pre = vmean;
    }

    return vmean;
}

bool DsoSnapshot::has_data(int index)
{
    if (_ch_enable.find(index) != _ch_enable.end())
        return _ch_enable[index];
    else
        return false;
}

int DsoSnapshot::get_block_num()
{
    const uint64_t size = _sample_count * get_unit_bytes() * get_channel_num();
    return (size >> LeafBlockPower) +
           ((size & LeafMask) != 0);
}

uint64_t DsoSnapshot::get_block_size(int block_index)
{
    assert(block_index < get_block_num());

    if (block_index < get_block_num() - 1) {
        return LeafBlockSamples;
    } else {
        const uint64_t size = _sample_count * get_unit_bytes() * get_channel_num();
        if (size % LeafBlockSamples == 0)
            return LeafBlockSamples;
        else
            return size % LeafBlockSamples;
    }
}

} // namespace data
} // namespace pv
