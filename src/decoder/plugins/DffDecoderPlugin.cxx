/*
 * Copyright (C) 2003-2018 The Music Player Daemon Project
 * http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include <sacd_media.h>
#include <sacd_reader.h>
#include <sacd_dsdiff.h>
#include <dst_decoder_mpd.h>
#undef MAX_CHANNELS
#include "DffDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "thread/Cond.hxx"
#include "thread/Mutex.hxx"
#include "util/Alloc.hxx"
#include "util/bit_reverse.h"
#include "util/FormatString.hxx"
#include "util/AllocatedString.hxx"
#include "util/UriUtil.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace std;

static const char* DSDIFF_TRACKXXX_FMT = "%cC_AUDIO__TRACK%03u.%3s";

static constexpr Domain dsdiff_domain("dsdiff");

static constexpr unsigned DST_DECODER_THREADS = 8;

static unsigned  param_dstdec_threads;
static bool      param_edited_master;
static bool      param_single_track;
static bool      param_lsbitfirst;
static area_id_e param_playable_area;
static bool      param_use_stdio;

static string         dsdiff_uri;
static sacd_media_t*  sacd_media  = nullptr;
static sacd_reader_t* sacd_reader = nullptr;

static unsigned
get_container_path_length(const char* path) {
	string container_path = path;
	container_path.resize(strrchr(container_path.c_str(), '/') - container_path.c_str());
	return container_path.length();
}

static string
get_container_path(const char* path) {
	string container_path = path;
	unsigned length = get_container_path_length(path);
	if (length >= 4) {
		container_path.resize(length);
		const char* c_str = container_path.c_str();
		if (strcasecmp(c_str + length - 4, ".dff") != 0) {
			container_path = path;
		}
	}
	return container_path;
}

static unsigned
get_subsong(const char* path) {
	unsigned length = get_container_path_length(path);
	if (length > 0) {
		const char* ptr = path + length + 1;
		char area = '\0';
		unsigned track_index = 0;
		char suffix[4];
		sscanf(ptr, DSDIFF_TRACKXXX_FMT, &area, &track_index, suffix);
		if (area == 'M') {
			track_index += sacd_reader->get_tracks(AREA_TWOCH);
		}
		track_index--;
		return track_index;
	}
	return 0;
}

static bool
dsdiff_update_toc(const char* path) {
	string curr_uri = path;
	if (path != nullptr) {
		if (!dsdiff_uri.compare(curr_uri)) {
			return true;
		}
	}
	else {
		if (dsdiff_uri.empty()) {
			return true;
		}
	}
	if (sacd_reader != nullptr) {
		sacd_reader->close();
		delete sacd_reader;
		sacd_reader = nullptr;
	}
	if (sacd_media != nullptr) {
		sacd_media->close();
		delete sacd_media;
		sacd_media = nullptr;
	}
	if (path != nullptr) {
		if (param_use_stdio) {
			sacd_media = new sacd_media_file_t();
		}
		else {
			sacd_media = new sacd_media_stream_t();
		}
		if (!sacd_media) {
			LogError(dsdiff_domain, "new sacd_media_t() failed");
			return false;
		}
		sacd_reader = new sacd_dsdiff_t;
		if (!sacd_reader) {
			LogError(dsdiff_domain, "new sacd_dsdiff_t() failed");
			return false;
		}
		if (!sacd_media->open(path)) {
			string err;
			err  = "sacd_media->open('";
			err += path;
			err += "') failed";
			LogWarning(dsdiff_domain, err.c_str());
			return false;
		}
		if (!sacd_reader->open(sacd_media, param_single_track ? MODE_SINGLE_TRACK : MODE_MULTI_TRACK)) {
			//LogWarning(dsdiff_domain, "sacd_reader->open(...) failed");
			return false;
		}
	}
	dsdiff_uri = curr_uri;
	return true;
}

static void
dsdiff_scan_info(unsigned track, TagHandler& handler) {
	string tag_value = to_string(track + 1);
	handler.OnTag(TAG_TRACK, tag_value.c_str());
	handler.OnDuration(SongTime::FromS(sacd_reader->get_duration(track)));
	sacd_reader->get_info(track, handler);
	auto track_format = sacd_reader->is_dst() ? "DST" : "DSD";
	handler.OnPair("codec", track_format);
}

static bool
dsdiff_init(const ConfigBlock& block) {
	param_dstdec_threads = block.GetBlockValue("dstdec_threads", DST_DECODER_THREADS);
	param_edited_master  = block.GetBlockValue("edited_master", false);
	param_single_track   = block.GetBlockValue("single_track", false);
	param_lsbitfirst     = block.GetBlockValue("lsbitfirst", false);
	const char* playable_area = block.GetBlockValue("playable_area", nullptr);
	param_playable_area = AREA_BOTH;
	if (playable_area != nullptr) {
		if (strcmp(playable_area, "stereo") == 0) {
			param_playable_area = AREA_TWOCH;
		}
		if (strcmp(playable_area, "multichannel") == 0) {
			param_playable_area = AREA_MULCH;
		}
	}
	param_use_stdio = block.GetBlockValue("use_stdio", false);
	return true;
}

static void
dsdiff_finish() noexcept {
	dsdiff_update_toc(nullptr);
}

static std::forward_list<DetachedSong>
dsdiff_container_scan(Path path_fs) {
	std::forward_list<DetachedSong> list;
	if (path_fs.IsNull()) {
		if (!param_single_track) {
			list.emplace_after(list.before_begin(), "multitrack_dsdiff");
		}
		return list;
	}
	if (!dsdiff_update_toc(path_fs.c_str())) {
		return list;
	}
	TagBuilder tag_builder;
	auto tail = list.before_begin();
	const char* suffix = uri_get_suffix(path_fs.c_str());
	char track_name[64];
	unsigned twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	unsigned mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	if (twoch_count + mulch_count < 2) {
		return list;
	}
	if (twoch_count > 0 && param_playable_area != AREA_MULCH) {
		sacd_reader->select_area(AREA_TWOCH);
		for (unsigned track = 0; track < twoch_count; track++) {
			AddTagHandler h(tag_builder);
			dsdiff_scan_info(track, h);
			sprintf(track_name, DSDIFF_TRACKXXX_FMT, '2', track + 1, suffix);
			tail = list.emplace_after(tail, track_name, tag_builder.Commit());
		}
	}
	if (mulch_count > 0 && param_playable_area != AREA_TWOCH) {
		sacd_reader->select_area(AREA_MULCH);
		for (unsigned track = 0; track < mulch_count; track++) {
			AddTagHandler h(tag_builder);
			dsdiff_scan_info(track, h);
			sprintf(track_name, DSDIFF_TRACKXXX_FMT, 'M', track + twoch_count + 1, suffix);
			tail = list.emplace_after(tail, track_name, tag_builder.Commit());
		}
	}
	return list;
}

static void
bit_reverse_buffer(uint8_t* p, uint8_t* end) {
	for (; p < end; ++p) {
		*p = bit_reverse(*p);
	}
}

static void
dsdiff_file_decode(DecoderClient &client, Path path_fs) {
	string path_container = get_container_path(path_fs.c_str());
	if (!dsdiff_update_toc(path_container.c_str())) {
		return;
	}
	unsigned twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	unsigned mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	unsigned track = (twoch_count + mulch_count > 1) ? get_subsong(path_fs.c_str()) : 0;

	// initialize reader
	sacd_reader->set_emaster(param_edited_master);
	if (track < twoch_count) {
		if (!sacd_reader->select_track(track, AREA_TWOCH, 0)) {
			LogError(dsdiff_domain, "cannot select track in stereo area");
			return;
		}
	}
	else {
		track -= twoch_count;
		if (track < sacd_reader->get_tracks(AREA_MULCH)) {
			if (!sacd_reader->select_track(track, AREA_MULCH, 0)) {
				LogError(dsdiff_domain, "cannot select track in multichannel area");
				return;
			}
		}
	}
	int dsd_channels = sacd_reader->get_channels();
	int dsd_samplerate = sacd_reader->get_samplerate();
	int dsd_framerate = sacd_reader->get_framerate();
	int dsd_buf_size = dsd_samplerate / 8 / dsd_framerate * dsd_channels;
	int dst_buf_size = dsd_samplerate / 8 / dsd_framerate * dsd_channels;
	vector<uint8_t> dsd_buf;
	vector<uint8_t> dst_buf;
	dsd_buf.resize(param_dstdec_threads * dsd_buf_size);
	dst_buf.resize(param_dstdec_threads * dst_buf_size);

	// initialize decoder
	AudioFormat audio_format = CheckAudioFormat(dsd_samplerate / 8, SampleFormat::DSD, dsd_channels);
	SongTime songtime = SongTime::FromS(sacd_reader->get_duration(track));
	client.Ready(audio_format, true, songtime);

	// play
	uint8_t* dsd_data;
	uint8_t* dst_data;
	size_t dsd_size = 0;
	size_t dst_size = 0;
	dst_decoder_t* dst_decoder = nullptr;
	auto cmd = client.GetCommand();
	for (;;) {
		int slot_nr = dst_decoder ? dst_decoder->get_slot_nr() : 0;
		dsd_data = dsd_buf.data() + dsd_buf_size * slot_nr;
		dst_data = dst_buf.data() + dst_buf_size * slot_nr;
		dst_size = dst_buf_size;
		frame_type_e frame_type;
		if (sacd_reader->read_frame(dst_data, &dst_size, &frame_type)) {
			if (dst_size > 0) {
				if (frame_type == FRAME_INVALID) {
					dst_size = dst_buf_size;
					memset(dst_data, 0xAA, dst_size);
				}
				if (frame_type == FRAME_DST) {
					if (!dst_decoder) {
						dst_decoder = new dst_decoder_t(param_dstdec_threads);
						if (!dst_decoder) {
							LogError(dsdiff_domain, "new dst_decoder_t() failed");
							break;
						}
						if (dst_decoder->init(dsd_channels, dsd_samplerate, dsd_framerate) != 0) {
							LogError(dsdiff_domain, "dst_decoder_t.init() failed");
							break;
						}
					}
					dst_decoder->decode(dst_data, dst_size, &dsd_data, &dsd_size);
				}
				else {
					dsd_data = dst_data;
					dsd_size = dst_size;
				}
				if (dsd_size > 0) {
					if (param_lsbitfirst) {
						bit_reverse_buffer(dsd_data, dsd_data + dsd_size);
					}
					cmd = client.SubmitData(nullptr, dsd_data, dsd_size, dsd_samplerate / 1000);
				}
			}
		}
		else {
			for (;;) {
				dst_data = nullptr;
				dst_size = 0;
				dsd_data = nullptr;
				dsd_size = 0;
				if (dst_decoder) {
					dst_decoder->decode(dst_data, dst_size, &dsd_data, &dsd_size);
				}
				if (dsd_size > 0) {
					if (param_lsbitfirst) {
						bit_reverse_buffer(dsd_data, dsd_data + dsd_size);
					}
					cmd = client.SubmitData(nullptr, dsd_data, dsd_size, dsd_samplerate / 1000);
					if (cmd == DecoderCommand::STOP || cmd == DecoderCommand::SEEK) {
						break;
					}
				}
				else {
					break;
				}
			}
			break;
		}
		if (cmd == DecoderCommand::STOP) {
			break;
		}
		if (cmd == DecoderCommand::SEEK) {
			double seconds = client.GetSeekTime().ToDoubleS();
			if (sacd_reader->seek(seconds)) {
				client.CommandFinished();
			}
			else {
				client.SeekError();
			}
			cmd = client.GetCommand();
		}
	}
	if (dst_decoder) {
		delete dst_decoder;
		dst_decoder = nullptr;
	}
}

static bool
dsdiff_scan_file(Path path_fs, TagHandler& handler) noexcept {
	string path_container = get_container_path(path_fs.c_str());
	if (path_container.empty() || !dsdiff_update_toc(path_container.c_str())) {
		return false;
	}
	unsigned twoch_count = sacd_reader->get_tracks(AREA_TWOCH);
	unsigned mulch_count = sacd_reader->get_tracks(AREA_MULCH);
	unsigned track = (twoch_count + mulch_count > 1) ? get_subsong(path_fs.c_str()) : 0;
	if (track < twoch_count) {
		sacd_reader->select_area(AREA_TWOCH);
	}
	else {
		track -= twoch_count;
		if (track < mulch_count) {
			sacd_reader->select_area(AREA_MULCH);
		}
		else {
			LogError(dsdiff_domain, "subsong index is out of range");
			return false;
		}
	}
	dsdiff_scan_info(track, handler);
	return true;
}

static const char* const dsdiff_suffixes[] = {
	"dff",
	nullptr
};

static const char* const dsdiff_mime_types[] = {
	"application/x-dff",
	"audio/x-dff",
	"audio/x-dsd",
	nullptr
};

extern const struct DecoderPlugin dff_decoder_plugin;
const struct DecoderPlugin dff_decoder_plugin = {
	"dsdiff",
	dsdiff_init,
	dsdiff_finish,
	nullptr,
	dsdiff_file_decode,
	dsdiff_scan_file,
	nullptr,
	dsdiff_container_scan,
	dsdiff_suffixes,
	dsdiff_mime_types,
};
