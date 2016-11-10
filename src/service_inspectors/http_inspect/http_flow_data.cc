//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http_flow_data.cc author Tom Peters <thopeter@cisco.com>

#include "http_enum.h"
#include "http_test_manager.h"
#include "http_flow_data.h"
#include "http_transaction.h"

using namespace HttpEnums;

unsigned HttpFlowData::http_flow_id = 0;

#ifdef REG_TEST
uint64_t HttpFlowData::instance_count = 0;
#endif

HttpFlowData::HttpFlowData() : FlowData(http_flow_id)
{
#ifdef REG_TEST
    if (HttpTestManager::use_test_output())
    {
        seq_num = ++instance_count;
        if (!HttpTestManager::use_test_input())
        {
            printf("Flow Data construct %" PRIu64 "\n", seq_num);
            fflush(nullptr);
        }
    }
#endif
}

HttpFlowData::~HttpFlowData()
{
#ifdef REG_TEST
    if (!HttpTestManager::use_test_input() && HttpTestManager::use_test_output())
    {
        printf("Flow Data destruct %" PRIu64 "\n", seq_num);
        fflush(nullptr);
    }
#endif
    for (int k=0; k <= 1; k++)
    {
        if ((section_type[k] != SEC_BODY_CHUNK) &&
            (section_type[k] != SEC_BODY_CL) &&
            (section_type[k] != SEC_BODY_OLD))
            // Body sections are reassembled in a static buffer
            delete[] section_buffer[k];
        HttpTransaction::delete_transaction(transaction[k]);
        delete cutter[k];
        if (compress_stream[k] != nullptr)
        {
            inflateEnd(compress_stream[k]);
            delete compress_stream[k];
        }
        if (mime_state[k] != nullptr)
        {
            delete mime_state[k];
        }
    }

    if (utf_state != nullptr )
    {
        delete utf_state;
    }

    delete_pipeline();
}

void HttpFlowData::half_reset(SourceId source_id)
{
    assert((source_id == SRC_CLIENT) || (source_id == SRC_SERVER));

    version_id[source_id] = VERS__NOT_PRESENT;
    data_length[source_id] = STAT_NOT_PRESENT;
    body_octets[source_id] = STAT_NOT_PRESENT;
    section_size_target[source_id] = 0;
    section_size_max[source_id] = 0;
    file_depth_remaining[source_id] = STAT_NOT_PRESENT;
    detect_depth_remaining[source_id] = STAT_NOT_PRESENT;
    compression[source_id] = CMP_NONE;
    if (compress_stream[source_id] != nullptr)
    {
        inflateEnd(compress_stream[source_id]);
        delete compress_stream[source_id];
        compress_stream[source_id] = nullptr;
    }
    if (mime_state[source_id] != nullptr)
    {
        delete mime_state[source_id];
        mime_state[source_id] = nullptr;
    }
    infractions[source_id].reset();
    events[source_id].reset();
    section_offset[source_id] = 0;
    chunk_state[source_id] = CHUNK_NUMBER;
    chunk_expected_length[source_id] = 0;

    if (source_id == SRC_CLIENT)
    {
        type_expected[SRC_CLIENT] = SEC_REQUEST;
        expected_trans_num[SRC_CLIENT]++;
        method_id = METH__NOT_PRESENT;
    }
    else
    {
        type_expected[SRC_SERVER] = SEC_STATUS;
        if (transaction[SRC_SERVER]->final_response())
            expected_trans_num[SRC_SERVER]++;
        status_code_num = STAT_NOT_PRESENT;
        if (utf_state != nullptr)
        {
            delete utf_state;
            utf_state = nullptr;
        }
    }
}

void HttpFlowData::trailer_prep(SourceId source_id)
{
    type_expected[source_id] = SEC_TRAILER;
    compression[source_id] = CMP_NONE;
    if (compress_stream[source_id] != nullptr)
    {
        inflateEnd(compress_stream[source_id]);
        delete compress_stream[source_id];
        compress_stream[source_id] = nullptr;
    }
    infractions[source_id].reset();
    events[source_id].reset();
}

bool HttpFlowData::add_to_pipeline(HttpTransaction* latest)
{
    if (pipeline == nullptr)
    {
        pipeline = new HttpTransaction*[MAX_PIPELINE];
    }
    assert(!pipeline_overflow && !pipeline_underflow);
    int new_back = (pipeline_back+1) % MAX_PIPELINE;
    if (new_back == pipeline_front)
    {
        pipeline_overflow = true;
        return false;
    }
    pipeline[pipeline_back] = latest;
    pipeline_back = new_back;
    return true;
}

HttpTransaction* HttpFlowData::take_from_pipeline()
{
    assert(!pipeline_underflow);
    if (pipeline_back == pipeline_front)
    {
        return nullptr;
    }
    int old_front = pipeline_front;
    pipeline_front = (pipeline_front+1) % MAX_PIPELINE;
    return pipeline[old_front];
}

void HttpFlowData::delete_pipeline()
{
    for (int k=pipeline_front; k != pipeline_back; k = (k+1) % MAX_PIPELINE)
    {
        HttpTransaction::delete_transaction(pipeline[k]);
    }
    delete[] pipeline;
}

#ifdef REG_TEST
void HttpFlowData::show(FILE* out_file) const
{
    assert(out_file != nullptr);
    fprintf(out_file, "Diagnostic output from HttpFlowData (Client/Server):\n");
    fprintf(out_file, "Version ID: %d/%d\n", version_id[0], version_id[1]);
    fprintf(out_file, "Method ID: %d\n", method_id);
    fprintf(out_file, "Status code: %d\n", status_code_num);
    fprintf(out_file, "Type expected: %d/%d\n", type_expected[0], type_expected[1]);
    fprintf(out_file, "Data length: %" PRIi64 "/%" PRIi64 "\n", data_length[0], data_length[1]);
    fprintf(out_file, "Detect depth remaining: %" PRIi64 "/%" PRIi64 "\n",
        detect_depth_remaining[0], detect_depth_remaining[1]);
    fprintf(out_file, "File depth remaining: %" PRIi64 "/%" PRIi64 "\n", file_depth_remaining[0],
        file_depth_remaining[1]);
    fprintf(out_file, "Body octets: %" PRIi64 "/%" PRIi64 "\n", body_octets[0], body_octets[1]);
    fprintf(out_file, "Pipelining: front %d back %d overflow %d underflow %d\n", pipeline_front,
        pipeline_back, pipeline_overflow, pipeline_underflow);
    fprintf(out_file, "Cutter: %s/%s\n", (cutter[0] != nullptr) ? "Present" : "nullptr",
        (cutter[1] != nullptr) ? "Present" : "nullptr");
    fprintf(out_file, "utf_state: %s\n", (utf_state != nullptr) ? "Present" : "nullptr");
    fprintf(out_file, "mime_state: %s/%s\n", (mime_state[0] != nullptr) ? "Present" : "nullptr",
        (mime_state[1] != nullptr) ? "Present" : "nullptr");
}
#endif

