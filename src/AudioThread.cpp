/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2014 Wang Bin <wbsecg1@gmail.com>

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

#include "AudioThread.h"
#include "AVThread_p.h"
#include "QtAV/AudioDecoder.h"
#include "QtAV/Packet.h"
#include "QtAV/AudioFormat.h"
#include "QtAV/AudioOutput.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/AVClock.h"
#include "output/OutputSet.h"
#include "QtAV/private/AVCompat.h"
#include <QtCore/QCoreApplication>
#include "utils/Logger.h"

namespace QtAV {

class AudioThreadPrivate : public AVThreadPrivate
{
public:
    void init() {
        resample = false;
        last_pts = 0;
    }

    bool resample;
    qreal last_pts; //used when audio output is not available, to calculate the aproximate sleeping time
};

AudioThread::AudioThread(QObject *parent)
    :AVThread(*new AudioThreadPrivate(), parent)
{
}

/// from libavfilter/af_volume begin
static inline void scale_samples_u8(quint8 *dst, const quint8 *src, int nb_samples, int volume, float)
{
	for (int i = 0; i < nb_samples; i++)
		dst[i] = av_clip_uint8(((((qint64)src[i] - 128) * volume + 128) >> 8) + 128);
}

static inline void scale_samples_u8_small(quint8 *dst, const quint8 *src, int nb_samples, int volume, float)
{
	for (int i = 0; i < nb_samples; i++)
		dst[i] = av_clip_uint8((((src[i] - 128) * volume + 128) >> 8) + 128);
}

static inline void scale_samples_s16(quint8 *dst, const quint8 *src, int nb_samples, int volume, float)
{
	int16_t *smp_dst = (int16_t *)dst;
	const int16_t *smp_src = (const int16_t *)src;
	for (int i = 0; i < nb_samples; i++)
		smp_dst[i] = av_clip_int16(((qint64)smp_src[i] * volume + 128) >> 8);
}

static inline void scale_samples_s16_small(quint8 *dst, const quint8 *src, int nb_samples, int volume, float)
{
	int16_t *smp_dst = (int16_t *)dst;
	const int16_t *smp_src = (const int16_t *)src;
	for (int i = 0; i < nb_samples; i++)
		smp_dst[i] = av_clip_int16((smp_src[i] * volume + 128) >> 8);
}

static inline void scale_samples_s32(quint8 *dst, const quint8 *src, int nb_samples, int volume, float)
{
	qint32 *smp_dst = (qint32 *)dst;
	const qint32 *smp_src = (const qint32 *)src;
	for (int i = 0; i < nb_samples; i++)
		smp_dst[i] = av_clipl_int32((((qint64)smp_src[i] * volume + 128) >> 8));
}
/// from libavfilter/af_volume end

template<typename T>
static inline void scale_samples(quint8 *dst, const quint8 *src, int nb_samples, int, float volume)
{
	T *smp_dst = (T *)dst;
	const T *smp_src = (const T *)src;
	for (int i = 0; i < nb_samples; ++i)
		smp_dst[i] = smp_src[i] * (T)volume;
}

typedef void(*scale_t)(quint8 *dst, const quint8 *src, int nb_samples, int volume, float volumef);

scale_t get_scaler(AudioFormat::SampleFormat fmt, qreal vol, int* voli)
{
	int v = (int)(vol * 256.0 + 0.5);
	if (voli)
		*voli = v;
	switch (fmt) {
	case AudioFormat::SampleFormat_Unsigned8:
	case AudioFormat::SampleFormat_Unsigned8Planar:
		return v < 0x1000000 ? scale_samples_u8_small : scale_samples_u8;
	case AudioFormat::SampleFormat_Signed16:
	case AudioFormat::SampleFormat_Signed16Planar:
		return v < 0x10000 ? scale_samples_s16_small : scale_samples_s16;
	case AudioFormat::SampleFormat_Signed32:
	case AudioFormat::SampleFormat_Signed32Planar:
		return scale_samples_s32;
	case AudioFormat::SampleFormat_Float:
	case AudioFormat::SampleFormat_FloatPlanar:
		return scale_samples<float>;
	case AudioFormat::SampleFormat_Double:
	case AudioFormat::SampleFormat_DoublePlanar:
		return scale_samples<double>;
	default:
		return 0;
	}
}

/*
 *TODO:
 * if output is null or dummy, the use duration to wait
 */
void AudioThread::run()
{
    DPTR_D(AudioThread);
    //No decoder or output. No audio output is ok, just display picture
    if (!d.dec || !d.dec->isAvailable() || !d.outputSet)
        return;
    resetState();
    Q_ASSERT(d.clock != 0);
    AudioDecoder *dec = static_cast<AudioDecoder*>(d.dec);
    AudioOutput *ao = 0;
    // first() is not null even if list empty
    if (!d.outputSet->outputs().isEmpty())
        ao = static_cast<AudioOutput*>(d.outputSet->outputs().first()); //TODO: not here
    d.init();
    //TODO: bool need_sync in private class
    bool is_external_clock = d.clock->clockType() == AVClock::ExternalClock;
    Packet pkt;
	scale_t scale = 0;
    while (!d.stop) {
        processNextTask();
        //TODO: why put it at the end of loop then playNextFrame() not work?
        if (tryPause()) { //DO NOT continue, or playNextFrame() will fail
            if (d.stop)
                break; //the queue is empty and may block. should setBlocking(false) wake up cond empty?
        } else {
            if (isPaused())
                continue;
        }
        if (d.packets.isEmpty() && !d.stop) {
            d.stop = d.demux_end;
        }
        if (d.stop) {
            qDebug("audio thread stop before take packet");
            break;
        }
        if (!pkt.isValid()) {
            pkt = d.packets.take(); //wait to dequeue
        }
        if (!pkt.isValid()) {
            qDebug("Invalid packet! flush audio codec context!!!!!!!! audio queue size=%d", d.packets.size());
            dec->flush();
            continue;
        }
        bool skip_render = pkt.pts < d.render_pts0;
        // audio has no key frame, skip rendering equals to skip decoding
        if (skip_render) {
            d.clock->updateValue(pkt.pts);
            /*
             * audio may be too fast than video if skip without sleep
             * a frame is about 20ms. sleep time must be << frame time
             */
            qreal a_v = pkt.pts - d.clock->videoPts();

            //qDebug("skip audio decode at %f/%f v=%f a-v=%fms", pkt.pts, d.render_pts0, d.clock->videoPts(), a_v*1000.0);
            if (a_v > 0)
                msleep(qMin((ulong)20, ulong(a_v*1000.0)));
            else
                msleep(1);
            pkt = Packet(); //mark invalid to take next
            continue;
        }
        d.render_pts0 = 0;
        if (is_external_clock) {
            d.delay = pkt.pts - d.clock->value();
            /*
             *after seeking forward, a packet may be the old, v packet may be
             *the new packet, then the d.delay is very large, omit it.
             *TODO: 1. how to choose the value
             * 2. use last delay when seeking
            */
            if (qAbs(d.delay) < 2.718) {
                if (d.delay < -kSyncThreshold) { //Speed up. drop frame?
                    //continue;
                }
                while (d.delay > kSyncThreshold) { //Slow down
                    //d.delay_cond.wait(&d.mutex, d.delay*1000); //replay may fail. why?
                    //qDebug("~~~~~wating for %f msecs", d.delay*1000);
                    usleep(kSyncThreshold * 1000000UL);
                    if (d.stop)
                        d.delay = 0;
                    else
                        d.delay -= kSyncThreshold;
                }
                if (d.delay > 0)
                    usleep(d.delay * 1000000UL);
            } else { //when to drop off?
                if (d.delay > 0) {
                    msleep(64);
                } else {
                    //audio packet not cleaned up?
                    continue;
                }
            }
        } else {
            d.clock->updateValue(pkt.pts);
        }
        //DO NOT decode and convert if ao is not available or mute!
        bool has_ao = ao && ao->isAvailable();
        //if (!has_ao) {//do not decode?
        // TODO: move resampler to AudioFrame, like VideoFrame does
        if (has_ao && dec->resampler()) {
            if (dec->resampler()->speed() != ao->speed()
                    || dec->resampler()->outAudioFormat() != ao->audioFormat()) {
                //resample later to ensure thread safe. TODO: test
                if (d.resample) {
                    qDebug() << "ao.format " << ao->audioFormat();
                    qDebug() << "swr.format " << dec->resampler()->outAudioFormat();
                    qDebug("decoder set speed: %.2f", ao->speed());
                    dec->resampler()->setOutAudioFormat(ao->audioFormat());
                    dec->resampler()->setSpeed(ao->speed());
                    dec->resampler()->prepare();
                    d.resample = false;
                } else {
                    d.resample = true;
                }
            }
        } else {
            if (dec->resampler() && dec->resampler()->speed() != d.clock->speed()) {
                if (d.resample) {
                    qDebug("decoder set speed: %.2f", d.clock->speed());
                    dec->resampler()->setSpeed(d.clock->speed());
                    dec->resampler()->prepare();
                    d.resample = false;
                } else {
                    d.resample = true;
                }
            }
        }
        if (d.stop) {
            qDebug("audio thread stop before decode()");
            break;
        }
        QMutexLocker locker(&d.mutex);
        Q_UNUSED(locker);
        if (!dec->decode(pkt.data)) {
            qWarning("Decode audio failed");
            qreal dt = pkt.pts - d.last_pts;
            if (dt > 0.618 || dt < 0) {
                dt = 0;
            }
            //qDebug("a sleep %f", dt);
            //TODO: avoid acummulative error. External clock?
            msleep((unsigned long)(dt*1000.0));
            pkt = Packet();
            d.last_pts = d.clock->value(); //not pkt.pts! the delay is updated!
            continue;
        }
        QByteArray decoded(dec->data());
        int decodedSize = decoded.size();
        int decodedPos = 0;
        qreal delay = 0;
        //AudioFormat.durationForBytes() calculates int type internally. not accurate
		const AudioFormat &af = dec->resampler()->outAudioFormat();
		const qreal byte_rate = af.bytesPerSecond();
		const qreal vol = ao->volume(); //keep const for 1 frame
		int volume_i = 0;
		if (has_ao) {
			scale = get_scaler(af.sampleFormat(), vol, &volume_i);
		}
        //AudioFormat &af = dec->resampler()->inAudioFormat();
        //qreal byte_rate = af.bytesPerSecond();
        while (decodedSize > 0) {
            if (d.stop) {
                qDebug("audio thread stop after decode()");
                break;
            }
            // TODO: set to format.bytesPerFrame()*1024?
            const int chunk = qMin(decodedSize, has_ao ? ao->bufferSize() : 1024*4);//int(max_len*byte_rate));
            //AudioFormat.bytesForDuration
            const qreal chunk_delay = (qreal)chunk/(qreal)byte_rate;
            pkt.pts += chunk_delay;
            QByteArray decodedChunk(chunk, 0); //volume == 0 || mute
            if (has_ao) {
                //TODO: volume filter and other filters!!!
                if (!ao->isMute()) {
                    decodedChunk = QByteArray::fromRawData(decoded.constData() + decodedPos, chunk);

					if (vol != 1.0 && scale) {
						// TODO: af_volume needs samples_align to get nb_samples
						const int nb_samples = decodedChunk.size() / ao->audioFormat().bytesPerSample();
						quint8 *dst = (quint8*)decodedChunk.constData();
						scale(dst, dst, nb_samples, volume_i, vol);
					}
                }
                ao->waitForNextBuffer();
                ao->receiveData(decodedChunk, pkt.pts);
                ao->play();
                d.clock->updateValue(ao->timestamp());

                emit frameDelivered(this);
            } else {
                d.clock->updateDelay(delay += chunk_delay);

            /*
             * why need this even if we add delay? and usleep sounds weird
             * the advantage is if no audio device, the play speed is ok too
             * So is portaudio blocking the thread when playing?
             */
                static bool sWarn_no_ao = true; //FIXME: no warning when replay. warn only once
                if (sWarn_no_ao) {
                    qDebug("Audio output not available! msleep(%lu)", (unsigned long)((qreal)chunk/(qreal)byte_rate * 1000));
                    sWarn_no_ao = false;
                }
                //TODO: avoid acummulative error. External clock?
                msleep((unsigned long)(chunk_delay * 1000.0));
            }
            decodedPos += chunk;
            decodedSize -= chunk;
        }
        int undecoded = dec->undecodedSize();
        if (undecoded > 0) {
            pkt.data.remove(0, pkt.data.size() - undecoded);
        } else {
            pkt = Packet();
        }

        d.last_pts = d.clock->value(); //not pkt.pts! the delay is updated!
    }
    d.packets.clear();
    qDebug("Audio thread stops running...");
}

} //namespace QtAV
