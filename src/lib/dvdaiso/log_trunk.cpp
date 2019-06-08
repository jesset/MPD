/*
* MPD DVD-Audio Decoder plugin
* Copyright (c) 2016 Maxim V.Anisiutkin <maxim.anisiutkin@gmail.com>
*
* DVD-Audio Decoder is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* DVD-Audio Decoder is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "util/Domain.hxx"
#include "Log.hxx"
#include "log_trunk.h"

extern "C" {
	#include "mlp_util.h"
}

static constexpr Domain mpd_domain("dvdaiso");

void mpd_av_log_callback(void* ptr, int level, const char* fmt, va_list vl) {
	(void)ptr;
	LogLevel mpd_level = LogLevel::DEFAULT;
	char msg[1024];
	switch (level) {
	case AV_LOG_PANIC:
	case AV_LOG_FATAL:
	case AV_LOG_ERROR:
		mpd_level = LogLevel::ERROR;
		break;
	case AV_LOG_WARNING:
		mpd_level = LogLevel::WARNING;
		break;
	case AV_LOG_INFO:
		mpd_level = LogLevel::INFO;
		break;
	case AV_LOG_VERBOSE:
	case AV_LOG_DEBUG:
		mpd_level = LogLevel::DEBUG;
		break;
	}
	vsprintf(msg, fmt, vl);
	#ifdef _DEBUG
	Log(mpd_level, mpd_domain, msg);
	#else
	if (level != AV_LOG_DEBUG) {
		Log(mpd_level, mpd_domain, msg);
	}
	#endif
}

void my_av_log_set_callback(void (*callback)(void*, int, const char*, va_list)) {
	mlp_av_log_set_callback(callback);
}

void my_av_log_set_default_callback(void) {
	mlp_av_log_set_callback(mlp_av_log_default_callback);
}

void mpd_dprintf(int ptr, const char* fmt, ...) {
	(void)ptr;
	LogLevel mpd_level = LogLevel::DEFAULT;
	char msg[1024];
	va_list vl;
	va_start(vl, fmt);
	vsprintf(msg, fmt, vl);
	Log(mpd_level, mpd_domain, msg);
	va_end(vl);
}
