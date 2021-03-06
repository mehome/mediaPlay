/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2014 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "AVPlayerPrivate.h"
#include "filter/FilterManager.h"
#include "output/OutputSet.h"
#include "QtAV/AudioDecoder.h"
#include "QtAV/AudioFormat.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/VideoCapture.h"
#include "QtAV/private/AVCompat.h"
#include "utils/Logger.h"

namespace QtAV {

static bool correct_audio_channels(AVCodecContext *ctx) {
    if (ctx->channels <= 0) {
        if (ctx->channel_layout) {
            ctx->channels = av_get_channel_layout_nb_channels(ctx->channel_layout);
        }
    } else {
        if (!ctx->channel_layout) {
            ctx->channel_layout = av_get_default_channel_layout(ctx->channels);
        }
    }
    return ctx->channel_layout > 0 && ctx->channels > 0;
}

AVPlayer::Private::Private()
    : auto_load(false)
    , async_load(true)
    , loaded(false)
    , fmt_ctx(0)
    , media_end(kInvalidPosition)
    , last_position(0)
    , reset_state(true)
    , start_position(0)
    , stop_position(kInvalidPosition)
    , repeat_max(0)
    , repeat_current(0)
    , timer_id(-1)
    , read_thread(0)
    , clock(new AVClock(AVClock::AudioClock))
    , vcapture(0)
    , speed(1.0)
    , brightness(0)
    , contrast(0)
    , saturation(0)
    , seeking(false)
    , seek_target(0)
    , interrupt_timeout(30000)
	, totalProgam(0)
{
	for (int i = 0; i < MAX_PROGRAM; i++)
	{
		vos[i] = 0;
		aos[i] = 0;
		adec[i] = 0;
		vdec[i] = 0;
		athread[i] = 0;
		vthread[i] = 0;
		ao[i] = 0;
		ao_enabled[i] = true;
		mute[i] = false;
	}

    demuxer.setInterruptTimeout(interrupt_timeout);
    /*
     * reset_state = true;
     * must be the same value at the end of stop(), and must be different from value in
     * stopFromDemuxerThread()(which is false), so the initial value must be true
     */

    vc_ids
#if QTAV_HAVE(DXVA)
            //<< VideoDecoderId_DXVA
#endif //QTAV_HAVE(DXVA)
#if QTAV_HAVE(VAAPI)
            //<< VideoDecoderId_VAAPI
#endif //QTAV_HAVE(VAAPI)
#if QTAV_HAVE(CEDARV)
            << VideoDecoderId_Cedarv
#endif //QTAV_HAVE(CEDARV)
            << VideoDecoderId_FFmpeg;
    ao_ids
#if QTAV_HAVE(OPENAL)
            << AudioOutputId_OpenAL
#endif
#if QTAV_HAVE(PORTAUDIO)
            << AudioOutputId_PortAudio
#endif
#if QTAV_HAVE(OPENSL)
            << AudioOutputId_OpenSL
#endif
#if QTAV_HAVE(DSound)
            << AudioOutputId_DSound
#endif
              ;
}
AVPlayer::Private::~Private() {
    // TODO: scoped ptr
	for (int i = 0; i < MAX_PROGRAM; i++)
	{
		if (ao[i]) {
			delete ao[i];
			ao[i] = 0;
		}
		if (adec[i]) {
			delete adec[i];
			adec[i] = 0;
		}
		if (vdec[i]) {
			delete vdec[i];
			vdec[i] = 0;
		}
		if (vos[i]) {
			vos[i]->clearOutputs();
			delete vos[i];
			vos[i] = 0;
		}
		if (aos[i]) {
			aos[i]->clearOutputs();
			delete aos[i];
			aos[i] = 0;
		}
	}
    if (vcapture) {
        delete vcapture;
        vcapture = 0;
    }
    if (clock) {
        delete clock;
        clock = 0;
    }
    if (read_thread) {
        delete read_thread;
        read_thread = 0;
    }
}


//TODO: av_guess_frame_rate in latest ffmpeg
void AVPlayer::Private::initStatistics(AVPlayer *player)
{
    statistics.reset();
    statistics.url = current_source.type() == QVariant::String ? current_source.toString() : QString();
    statistics.bit_rate = fmt_ctx->bit_rate;
    statistics.format = fmt_ctx->iformat->name;
    //AV_TIME_BASE_Q: msvc error C2143
    //fmt_ctx->duration may be AV_NOPTS_VALUE. AVDemuxer.duration deals with this case
    statistics.start_time = QTime(0, 0, 0).addMSecs(int(player->mediaStartPosition()));
    statistics.duration = QTime(0, 0, 0).addMSecs((int)player->duration());

	// add later
	/*
    if (vdec)
        statistics.video.decoder = VideoDecoderFactory::name(vdec->id()).c_str();
	*/

    statistics.metadata.clear();
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        statistics.metadata.insert(tag->key, tag->value);
    }
    struct common_statistics_t {
        int stream_idx;
        AVCodecContext *ctx;
        Statistics::Common *st;
        const char *name;
    } common_statistics[] = {
        { demuxer.videoStream(), demuxer.videoCodecContext(), &statistics.video, "video"},
        { demuxer.audioStream(), demuxer.audioCodecContext(), &statistics.audio, "audio"},
        { 0, 0, 0, 0}
    };
    for (int i = 0; common_statistics[i].name; ++i) {
        common_statistics_t cs = common_statistics[i];
        if (cs.stream_idx < 0)
            continue;
        AVStream *stream = fmt_ctx->streams[cs.stream_idx];
        qDebug("stream: %d, duration=%lld (%lld ms==%lld), time_base=%f", cs.stream_idx, stream->duration, qint64(qreal(stream->duration)*av_q2d(stream->time_base)*1000.0)
               , player->duration(), av_q2d(stream->time_base));
        cs.st->available = true;
        cs.st->codec = avcodec_get_name(cs.ctx->codec_id);
        if (cs.ctx->codec) {
            cs.st->codec_long = cs.ctx->codec->long_name;
        }
        cs.st->total_time = QTime(0, 0, 0).addMSecs(int(qreal(stream->duration)*av_q2d(stream->time_base)*1000.0));
        cs.st->start_time = QTime(0, 0, 0).addMSecs(int(qreal(stream->start_time)*av_q2d(stream->time_base)*1000.0));
        qDebug("codec: %s(%s)", qPrintable(cs.st->codec), qPrintable(cs.st->codec_long));
        cs.st->bit_rate = cs.ctx->bit_rate; //fmt_ctx
        cs.st->frames = stream->nb_frames;
        //qDebug("time: %f~%f, nb_frames=%lld", cs.st->start_time, cs.st->total_time, stream->nb_frames); //why crash on mac? av_q2d({0,0})?
        tag = NULL;
        while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            cs.st->metadata.insert(tag->key, tag->value);
        }
    }
    if (demuxer.audioStream() >= 0) {
        AVCodecContext *aCodecCtx = demuxer.audioCodecContext();
        correct_audio_channels(aCodecCtx);
        statistics.audio_only.block_align = aCodecCtx->block_align;
        statistics.audio_only.channels = aCodecCtx->channels;
        char cl[128]; //
        // nb_channels -1: will use av_get_channel_layout_nb_channels
        av_get_channel_layout_string(cl, sizeof(cl), aCodecCtx->channels, aCodecCtx->channel_layout); //TODO: ff version
        statistics.audio_only.channel_layout = cl;
        statistics.audio_only.sample_fmt = av_get_sample_fmt_name(aCodecCtx->sample_fmt);
        statistics.audio_only.frame_number = aCodecCtx->frame_number;
        statistics.audio_only.frame_size = aCodecCtx->frame_size;
        statistics.audio_only.sample_rate = aCodecCtx->sample_rate;
    }
    if (demuxer.videoStream() >= 0) {
        AVCodecContext *vCodecCtx = demuxer.videoCodecContext();
        AVStream *stream = fmt_ctx->streams[demuxer.videoStream()];
        statistics.video.frames = stream->nb_frames;
        //FIXME: which 1 should we choose? avg_frame_rate may be nan, r_frame_rate may be wrong(guessed value)
        // TODO: seems that r_frame_rate will be removed libav > 9.10. Use macro to check version?
        //if (stream->avg_frame_rate.num) //avg_frame_rate.num,den may be 0
            statistics.video_only.fps_guess = av_q2d(stream->avg_frame_rate);
        //else
        //    statistics.video_only.fps_guess = av_q2d(stream->r_frame_rate);
        statistics.video_only.fps = statistics.video_only.fps_guess;
        statistics.video_only.avg_frame_rate = av_q2d(stream->avg_frame_rate);
        statistics.video_only.coded_height = vCodecCtx->coded_height;
        statistics.video_only.coded_width = vCodecCtx->coded_width;
        statistics.video_only.gop_size = vCodecCtx->gop_size;
        statistics.video_only.pix_fmt = av_get_pix_fmt_name(vCodecCtx->pix_fmt);
        statistics.video_only.height = vCodecCtx->height;
        statistics.video_only.width = vCodecCtx->width;
    }
}

bool AVPlayer::Private::setupAudioThread(AVPlayer *player)
{
	QList<int> audioList = demuxer.audioStreams();

	int index = 0;
	for (QList<int>::iterator it = audioList.begin(); it != audioList.end(); it++, index++)
	{
		// TODO all audio will be add
		AVCodecContext *aCodecCtx = demuxer.audioCodecContext(*it);
		if (!aCodecCtx || aCodecCtx->bit_rate <= 0 || aCodecCtx->sample_rate <= 0) {
			//return false;
			index--;
			demuxer.removeAudioStream(*it);
			continue;
		}
		qDebug("has audio");
		if (!adec[index]) {
			adec[index] = new AudioDecoder();
			connect(adec[index], SIGNAL(error(QtAV::AVError)), player, SIGNAL(error(QtAV::AVError)));
		}
		adec[index]->setCodecContext(aCodecCtx);
		adec[index]->setOptions(ac_opt);
		if (!adec[index]->open()) {
			AVError e(AVError::AudioCodecNotFound);
			qWarning() << e.string();
			emit player->error(e);
			return false;
		}
		//TODO: setAudioOutput() like vo
		if (!ao[index] && ao_enabled) {
			foreach(AudioOutputId aoid, ao_ids) {
				qDebug("trying audio output '%s'", AudioOutputFactory::name(aoid).c_str());
				ao[index] = AudioOutputFactory::create(aoid);
				if (ao[index]) {
					qDebug("audio output found.");
					break;
				}
			}
		}
		if (!ao[index]) {
			// TODO: only when no audio stream or user disable audio stream. running an audio thread without sound is waste resource?
			//masterClock()->setClockType(AVClock::ExternalClock);
			//return;
		}
		else {
			ao[index]->close();
			correct_audio_channels(aCodecCtx);
			AudioFormat af;
			af.setSampleRate(aCodecCtx->sample_rate);
			af.setSampleFormatFFmpeg(aCodecCtx->sample_fmt);
			// 5, 6, 7 channels may not play
			if (aCodecCtx->channels > 2)
				af.setChannelLayout(ao[index]->preferredChannelLayout());
			else
				af.setChannelLayoutFFmpeg(aCodecCtx->channel_layout);
			//af.setChannels(aCodecCtx->channels);
			// FIXME: workaround. planar convertion crash now!
			if (af.isPlanar()) {
				af.setSampleFormat(ao[index]->preferredSampleFormat());
			}
			if (!ao[index]->isSupported(af)) {
				if (!ao[index]->isSupported(af.sampleFormat())) {
					af.setSampleFormat(ao[index]->preferredSampleFormat());
				}
				if (!ao[index]->isSupported(af.channelLayout())) {
					af.setChannelLayout(ao[index]->preferredChannelLayout());
				}
			}
			ao[index]->setAudioFormat(af);
			if (!ao[index]->open()) {
				//could not open audio device. use extrenal clock
				delete ao[index];
				ao[index] = 0;
				// audio can not find, ignore this audio
				index--;
				demuxer.removeAudioStream(*it);
				continue;
				//return false;
			}
		}
		if (ao[index])
			adec[index]->resampler()->setOutAudioFormat(ao[index]->audioFormat());
		adec[index]->resampler()->inAudioFormat().setSampleFormatFFmpeg(aCodecCtx->sample_fmt);
		adec[index]->resampler()->inAudioFormat().setSampleRate(aCodecCtx->sample_rate);
		adec[index]->resampler()->inAudioFormat().setChannels(aCodecCtx->channels);
		adec[index]->resampler()->inAudioFormat().setChannelLayoutFFmpeg(aCodecCtx->channel_layout);
		adec[index]->prepare();
		if (!athread[index]) {
			qDebug("new audio thread");
			athread[index] = new AudioThread(player);
			athread[index]->setClock(clock);
			// just the first program add statistics
			if (index == 0)
				athread[index]->setStatistics(&statistics);
			athread[index]->setOutputSet(aos[index]);
			qDebug("demux thread setAudioThread");
			read_thread->setAudioThread(athread[index], *it, index);
			//reconnect if disconnected
			QList<Filter*> filters = FilterManager::instance().audioFilters(player);
			//TODO: isEmpty()==false but size() == 0 in debug mode, it's a Qt bug? we can not just foreach without check empty in debug mode
			if (filters.size() > 0) {
				foreach(Filter *filter, filters) {
					athread[index]->installFilter(filter);
				}
			}
		}
		athread[index]->setDecoder(adec[index]);
		player->setAudioOutput(ao[index],index);
		int queue_min = 0.61803*qMax<qreal>(24.0, statistics.video_only.fps_guess);
		int queue_max = int(1.61803*(qreal)queue_min); //about 1 second
		athread[index]->packetQueue()->setThreshold(queue_min);
		athread[index]->packetQueue()->setCapacity(queue_max);
	}
    return true;
}

bool AVPlayer::Private::setupVideoThread(AVPlayer *player)
{
	QList<int> videoList = demuxer.videoStreams();

	int index = 0;
	for (QList<int>::iterator it = videoList.begin(); it != videoList.end(); it++, index++)
	{
		AVCodecContext *vCodecCtx = demuxer.videoCodecContext(*it);
		if (!vCodecCtx || vCodecCtx->width <= 0 || vCodecCtx->height <= 0) {
			//return false;
			index--;
			demuxer.removeVideoStream(*it);
			continue;
		}
		/*
		if (!vdec) {
		vdec = VideoDecoderFactory::create(VideoDecoderId_FFmpeg);
		}
		*/
		if (vdec[index]) {
			vdec[index]->disconnect();
			delete vdec[index];
			vdec[index] = 0;
		}
		foreach(VideoDecoderId vid, vc_ids) {
			qDebug("**********trying video decoder: %s...", VideoDecoderFactory::name(vid).c_str());
			VideoDecoder *vd = VideoDecoderFactory::create(vid);
			if (!vd) {
				continue;
			}
			//vd->isAvailable() //TODO: the value is wrong now
			vd->setCodecContext(vCodecCtx);
			vd->setOptions(vc_opt);
			if (vd->prepare() && vd->open()) {
				vdec[index] = vd;
				qDebug("**************Video decoder found");
				break;
			}
			delete vd;
		}
		if (!vdec) {
			// DO NOT emit error signals in VideoDecoder::open(). 1 signal is enough
			AVError e(AVError::VideoCodecNotFound);
			qWarning() << e.string();
			emit player->error(e);
			return false;
		}
		connect(vdec[index], SIGNAL(error(QtAV::AVError)), player, SIGNAL(error(QtAV::AVError)));

		if (!vthread[index]) {
			vthread[index] = new VideoThread(player);
			vthread[index]->setClock(clock);
			// just the first program add statistics
			if (index == 0)
				vthread[index]->setStatistics(&statistics);
			vthread[index]->setVideoCapture(vcapture);
			vthread[index]->setOutputSet(vos[index]);
			read_thread->setVideoThread(vthread[index], *it, index);

			QList<Filter*> filters = FilterManager::instance().videoFilters(player);
			if (filters.size() > 0) {
				foreach(Filter *filter, filters) {
					vthread[index]->installFilter(filter);
				}
			}
		}
		vthread[index]->setDecoder(vdec[index]);
		vthread[index]->setBrightness(brightness);
		vthread[index]->setContrast(contrast);
		vthread[index]->setSaturation(saturation);
		int queue_min = 0.61803*qMax<qreal>(24.0, statistics.video_only.fps_guess);
		int queue_max = int(1.61803*(qreal)queue_min); //about 1 second
		vthread[index]->packetQueue()->setThreshold(queue_min);
		vthread[index]->packetQueue()->setCapacity(queue_max);
	}
    return true;
}

} //namespace QtAV
