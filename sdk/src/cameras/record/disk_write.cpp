// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2016 Intel Corporation. All Rights Reserved.

#include <stddef.h>
#include "disk_write.h"
#include "include/file.h"
#include "rs_sdk_version.h"
#include "rs/utils/log_utils.h"
#include <stddef.h>

using namespace rs::core;

namespace rs
{
    namespace record
    {
        static const uint32_t MAX_CACHED_SAMPLES = 5;

        disk_write::disk_write(void):
            m_is_configured(false),
            m_paused(false),
            m_stop_writing(true),
            m_min_fps(0)
        {

        }

        disk_write::~disk_write(void)
        {
            stop();
        }

        uint32_t disk_write::get_min_fps(const std::map<rs_stream, core::file_types::stream_profile>& stream_profiles)
        {
            uint32_t rv = 0xffffffff;
            for(auto it = stream_profiles.begin(); it != stream_profiles.end(); ++it)
            {
                if(static_cast<uint32_t>(it->second.info.framerate) < rv)
                    rv = it->second.info.framerate;
            }
            if(rv == 0)
                throw std::runtime_error("illegal frame rate value");
            if(rv == 0xffffffff)
                throw std::runtime_error("no streams were enabled before start streaming");
            return rv;
        }

        bool disk_write::allow_sample(std::shared_ptr<rs::core::file_types::sample> &sample)
        {
            if(sample->info.type != file_types::sample_type::st_image) return true;
            auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(sample);
            if (frame)
            {
                auto max_samples = MAX_CACHED_SAMPLES * frame->finfo.framerate / m_min_fps;
                if(m_samples_count[frame->finfo.stream] > max_samples) return false;
                m_samples_count[frame->finfo.stream]++;
                return true;
            }
            return false;
        }

        void disk_write::record_sample(std::shared_ptr<file_types::sample> &sample)
        {
            LOG_FUNC_SCOPE();
            if (m_paused)
            {
                return;//device is still streaming but samples are not recorded
            }
            bool insert_samples = false;
            {
                std::lock_guard<std::mutex> guard(m_main_mutex);
                insert_samples = allow_sample(sample);
                if (insert_samples)//it is ok that sample queue size may exceed MAX_CACHED_SAMPLES by few samples
                {
                    m_samples_queue.push(sample);
                }
                else
                {
                    LOG_WARN("sample drop, sample type - " << sample->info.type << " ,capture time - " << sample->info.capture_time);
                }
            }
            if(insert_samples)
            {
                std::lock_guard<std::mutex> guard(m_notify_write_thread_mutex);
                m_notify_write_thread_cv.notify_one();
            }
        }

        bool disk_write::start()
        {
            LOG_FUNC_SCOPE();
            if(!m_is_configured) return false;
            m_stop_writing = false;//protection is not required before the thread is started
            assert(!m_thread.joinable());//we don't expect the thread to be active on start
            m_thread = std::thread(&disk_write::write_thread, this);
            return true;
        }

        void disk_write::stop()
        {
            LOG_FUNC_SCOPE();

            {
                std::lock_guard<std::mutex> guard(m_main_mutex);
                m_stop_writing = true;
                std::queue<std::shared_ptr<core::file_types::sample>> empty_queue;
                std::swap(m_samples_queue, empty_queue);

                if(m_file)
                    m_file->close();
            }

            {
                std::lock_guard<std::mutex> guard(m_notify_write_thread_mutex);
                m_notify_write_thread_cv.notify_one();
            }

            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }

        void disk_write::set_pause(bool pause)
        {
            std::lock_guard<std::mutex> guard(m_main_mutex);
            std::queue<std::shared_ptr<core::file_types::sample>> empty_queue;
            std::swap(m_samples_queue, empty_queue);
            m_paused = pause;
        }

        status disk_write::configure(const configuration& config)
        {
            std::lock_guard<std::mutex> guard(m_main_mutex);
            if(m_is_configured) return status::status_exec_aborted;
            m_file = std::unique_ptr<rs::core::file>(new rs::core::file());
            status sts = m_file->open(config.m_file_path, (open_file_option)(open_file_option::write));

            if (sts != status::status_no_error)
                return sts;

            m_min_fps = get_min_fps(config.m_stream_profiles);
            write_header(config.m_stream_profiles.size(), config.m_coordinate_system);
            write_device_info(config.m_device_info);
            write_sw_info();
            write_capabilities(config.m_capabilities);
            write_motion_intrinsics(config.m_motion_intrinsics);
            write_stream_info(config.m_stream_profiles);
            write_properties(config.m_options);
            write_first_frame_offset();
            m_is_configured = true;
            return sts;
        }

        void disk_write::write_thread(void)
        {
            LOG_FUNC_SCOPE();
            while (!m_stop_writing)
            {
                std::unique_lock<std::mutex> guard(m_notify_write_thread_mutex);
                m_notify_write_thread_cv.wait(guard);
                guard.unlock();

                LOG_VERBOSE("queue contains " << m_samples_queue.size() << " samples")

                std::shared_ptr<core::file_types::sample> sample = nullptr;
                while(!m_samples_queue.empty())
                {
                    {
                        std::lock_guard<std::mutex> guard(m_main_mutex);
                        if(m_samples_queue.empty()) break;
                        sample = m_samples_queue.front();
                        m_samples_queue.pop();
                        assert(sample);
                    }
                    write_sample_info(sample);
                    write_sample(sample);
                }
            }
            /* Write the stream numbers */
            write_stream_num_of_frames();
        }

        void disk_write::write_header(uint8_t stream_count, file_types::coordinate_system cs)
        {
            file_types::disk_format::file_header header = {};
            header.data.version = 2;
            header.data.id = UID('R', 'S', 'L', '0' + header.data.version);
            header.data.coordinate_system = cs;

            /* calculate the number of streams */
            header.data.nstreams = stream_count;

            uint32_t bytes_written = 0;
            m_file->set_position(0, move_method::begin);
            m_file->write_bytes(&header, sizeof(header), bytes_written);
            LOG_INFO("write header chunk, chunk size - " << sizeof(header))
        }

        void disk_write::write_device_info(file_types::device_info info)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_device_info;
            file_types::disk_format::device_info device_info;
            chunk.size = sizeof(device_info);
            device_info.data = info;

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(&device_info, sizeof(device_info), bytes_written);
            LOG_INFO("write device info chunk, chunk size - " << chunk.size)
        }

        void disk_write::write_sw_info()
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_sw_info;
            chunk.size = sizeof(file_types::disk_format::sw_info);

            file_types::disk_format::sw_info sw_info;
            memset(&sw_info, 0, sizeof(file_types::disk_format::sw_info));
            sw_info.data.sdk = {SDK_VER_MAJOR, SDK_VER_MINOR, SDK_VER_COMMIT_NUMBER, SDK_VER_COMMIT_ID};
            sw_info.data.librealsense = {RS_API_MAJOR_VERSION, RS_API_MINOR_VERSION, RS_API_PATCH_VERSION};

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(&sw_info, sizeof(sw_info), bytes_written);
            LOG_INFO("write sw info chunk, chunk size - " << chunk.size)
        }

        void disk_write::write_capabilities(std::vector<rs_capabilities> capabilities)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_capabilities;
            chunk.size = capabilities.size() * sizeof(rs_capabilities);

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(capabilities.data(), chunk.size, bytes_written);
            LOG_INFO("write capabilities chunk, chunk size - " << chunk.size)
        }

        void disk_write::write_stream_info(std::map<rs_stream, file_types::stream_profile> profiles)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_stream_info;
            chunk.size = profiles.size() * sizeof(file_types::disk_format::stream_info);

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);

            /* Write each stream info */

            for(auto iter = profiles.begin(); iter != profiles.end(); ++iter)
            {
                file_types::stream_info sinfo = {};
                auto stream = iter->first;
                sinfo.ctype = m_compression.compression_policy(stream);
                sinfo.profile = iter->second;
                /* Save the stream nframes offset for later update */
                int64_t pos = 0;
                m_file->set_position(pos, move_method::current, &pos);
                m_offsets[stream] = pos + offsetof(file_types::stream_info, nframes);
                sinfo.stream = stream;
                file_types::disk_format::stream_info stream_info = {};
                stream_info.data = sinfo;
                m_file->write_bytes(&stream_info, sizeof(stream_info), bytes_written);
                LOG_INFO("write stream info chunk, chunk size - " << chunk.size)
            }
        }

        void disk_write::write_motion_intrinsics(const rs_motion_intrinsics &motion_intrinsics)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_motion_intrinsics;
            file_types::disk_format::motion_intrinsics mi;
            chunk.size = sizeof(mi);
            mi.data = motion_intrinsics;

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(&mi, chunk.size, bytes_written);
            LOG_INFO("write motion intrinsics chunk, chunk size - " << chunk.size)
        }

        void disk_write::write_properties(const std::vector<file_types::device_cap>& properties)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_properties;
            chunk.size = properties.size()*sizeof(file_types::device_cap);

            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(properties.data(), chunk.size, bytes_written);
            LOG_INFO("write properties chunk, chunk size - " << chunk.size)
        }

        void disk_write::write_first_frame_offset()
        {
            int64_t pos = 0;
            m_file->set_position(pos, move_method::current, &pos);
            m_file->set_position((int64_t)offsetof(file_types::file_header, first_frame_offset), move_method::begin);

            uint32_t bytes_written = 0;
            int32_t firstFramePosition = (int32_t)pos;
            m_file->write_bytes(&firstFramePosition, sizeof(firstFramePosition), bytes_written);
            m_file->set_position(pos, move_method::begin, &pos);
            LOG_INFO("first frame offset - " << pos)

        }

        void disk_write::write_stream_num_of_frames()
        {
            for (auto s = m_offsets.begin(); s != m_offsets.end(); s++)
            {
                auto itr = m_number_of_frames.find(s->first);
                if (itr == m_number_of_frames.end()) continue;

                uint32_t bytes_written = 0;
                m_file->set_position(s->second, move_method::begin);
                m_file->write_bytes(&itr->second, sizeof(int32_t), bytes_written);
                LOG_INFO("stream - " << s->first << " ,number of frames - " << itr->second)
            }
        }

        void disk_write::write_sample_info(std::shared_ptr<file_types::sample> &sample)
        {
            file_types::chunk_info chunk = {};
            chunk.id = file_types::chunk_id::chunk_sample_info;
            chunk.size = sizeof(file_types::disk_format::sample_info);
            file_types::disk_format::sample_info sample_info;

            uint64_t pos = 0;
            m_file->get_position(&pos);
            sample->info.offset = pos;

            sample_info.data = sample->info;
            uint32_t bytes_written = 0;
            m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
            m_file->write_bytes(&sample_info, chunk.size, bytes_written);
        }

        void disk_write::write_sample(std::shared_ptr<file_types::sample> &sample)
        {
            switch(sample->info.type)
            {
                case file_types::sample_type::st_image:
                {
                    file_types::chunk_info chunk = {};
                    chunk.id = file_types::chunk_id::chunk_frame_info;
                    file_types::disk_format::frame_info frame_info = {};
                    chunk.size = sizeof(frame_info);
                    auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(sample);
                    if (frame)
                    {
                        m_number_of_frames[frame->finfo.stream]++;
                        frame_info.data = frame->finfo;

                        uint32_t bytes_written = 0;
                        m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
                        m_file->write_bytes(&frame_info, chunk.size, bytes_written);
                        write_image_data(sample);
                        LOG_VERBOSE("write frame, stream type - " << frame->finfo.stream << " capture time - " << frame->info.capture_time);
                        LOG_VERBOSE("write frame, stream type - " << frame->finfo.stream << " system time - " << frame->finfo.system_time);
                        LOG_VERBOSE("write frame, stream type - " << frame->finfo.stream << " time stamp - " << frame->finfo.time_stamp);
                        LOG_VERBOSE("write frame, stream type - " << frame->finfo.stream << " frame number - " << frame->finfo.number);
                    }
                    break;
                }
                case file_types::sample_type::st_motion:
                {
                    file_types::chunk_info chunk = {};
                    chunk.id = file_types::chunk_id::chunk_sample_data;
                    file_types::disk_format::motion_data motion_data = {};
                    chunk.size = sizeof(motion_data);
                    auto motion = std::dynamic_pointer_cast<file_types::motion_sample>(sample);
                    if (motion)
                    {
                        motion_data.data = motion->data;
                        uint32_t bytes_written = 0;
                        m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
                        m_file->write_bytes(&motion_data, chunk.size, bytes_written);
                        LOG_VERBOSE("write motion, relative time - " << motion->info.capture_time)
                    }
                    break;
                }
                case file_types::sample_type::st_time:
                {
                    file_types::chunk_info chunk = {};
                    chunk.id = file_types::chunk_id::chunk_sample_data;
                    file_types::disk_format::time_stamp_data time_stamp_data = {};
                    chunk.size = sizeof(time_stamp_data);
                    auto time = std::dynamic_pointer_cast<file_types::time_stamp_sample>(sample);
                    if (time)
                    {
                        time_stamp_data.data = time->data;
                        uint32_t bytes_written = 0;
                        m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
                        m_file->write_bytes(&time_stamp_data, chunk.size, bytes_written);
                        LOG_VERBOSE("write time stamp, relative time - " << time->info.capture_time)
                    }
                    break;
                }
            }
        }

        void disk_write::write_image_data(std::shared_ptr<file_types::sample> &sample)
        {
            auto frame = std::dynamic_pointer_cast<file_types::frame_sample>(sample);

            if (frame)
            {
                /* Get raw stream size */
                int32_t nbytes = (frame->finfo.stride * frame->finfo.height);

                std::vector<uint8_t> buffer;
                file_types::compression_type ctype = m_compression.compression_policy(frame->finfo.stream);
                if (ctype == file_types::compression_type::none)
                {
                    auto data = frame->data;
                    buffer = std::vector<uint8_t>(data, data + nbytes);
                }
                else
                {
                    m_compression.encode_image(ctype, frame, buffer);
                }

                file_types::chunk_info chunk = {};
                chunk.id = file_types::chunk_id::chunk_sample_data;
                chunk.size = buffer.size();

                u_int32_t bytes_written = 0;
                m_file->write_bytes(&chunk, sizeof(chunk), bytes_written);
                m_file->write_bytes(buffer.data(), chunk.size, bytes_written);
                std::lock_guard<std::mutex> guard(m_main_mutex);
                m_samples_count[frame->finfo.stream]--;
            }
        }
    }
}