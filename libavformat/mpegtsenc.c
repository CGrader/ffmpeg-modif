/*
 * MPEG2 transport stream (aka DVB) muxer
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
 
 /* Com relação às modificações necessárias para adpatação ao sistema brasileiro de televisão digital,
 são consultadas para referência as seguintes normas:

ISO/IEC 13818-1: (padrão internacional) Information technology — Generic coding
of moving pictures and associated audio information: Systems

ABNT NBR 15603-3: Televisão digital terrestre — Multiplexação e serviços de informação (SI) —
Parte 3: Sintaxes e definições de informação
estendida do SI   */

 

#include "libavutil/bswap.h"
#include "libavutil/crc.h"
#include "libavutil/dict.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavcodec/internal.h"
#include "avformat.h"
#include "internal.h"
#include "mpegts.h"

#define PCR_TIME_BASE 27000000

/* write DVB SI sections */

/*********************************************/
/* mpegts section writer */

/* Estrutura que representa uma seção de PSI ou SIe contém informação dos PIDS das tabelas, status
do continuity conunter e um ponteiro pra variável opaque da estrutura AVFormatContext */


typedef struct MpegTSSection {
    int pid;
    int cc;
    void (*write_packet)(struct MpegTSSection *s, const uint8_t *packet);
    void *opaque;
} MpegTSSection;

/* Representa um serviço e contém uma MpegTSSection pra sua PMT, um ID do serviço, o PCR PID e variáveis
de controle p/ gerenciar a taxa de transmissão do PCR. */

typedef struct MpegTSService {
    MpegTSSection pmt; /* MPEG2 pmt table context */
    int sid;           /* service ID */
    char *name;
    char *provider_name;
    int pcr_pid;
    int pcr_packet_count;
    int pcr_packet_period;
} MpegTSService;

typedef struct MpegTSWrite {
    const AVClass *av_class;
    MpegTSSection pat; /* MPEG2 pat table */
    MpegTSSection nit; /* MPEG2 nit table */
    MpegTSSection sdt; /* MPEG2 sdt table context */
    MpegTSSection tot; /* MPEG2 sdt table context */
    MpegTSService **services;
    int sdt_packet_count;
    int sdt_packet_period;
    int nit_packet_count;
    int nit_packet_period;
	int tot_packet_count;
    int tot_packet_period;
    int pat_packet_count;
    int pat_packet_period;
    int nb_services;
    int final_nb_services;
    int area_code;
    int guard_interval;
    int transmission_mode;
    int physical_channel;
    int virtual_channel;
    int transmission_profile;
    int onid;
    int tsid;
    int64_t first_pcr;
    int mux_rate; ///< set to 1 when VBR
    int pes_payload_size;

    int transport_stream_id;
    int original_network_id;
    int service_id;

    int pmt_start_pid;
    int start_pid;
    int m2ts_mode;

    int reemit_pat_pmt; // backward compatibility

#define MPEGTS_FLAG_REEMIT_PAT_PMT  0x01
#define MPEGTS_FLAG_AAC_LATM        0x02
    int flags;
    int copyts;
    int tables_version;
} MpegTSWrite;

/* a PES packet header is generated every DEFAULT_PES_HEADER_FREQ packets */
#define DEFAULT_PES_HEADER_FREQ 16
#define DEFAULT_PES_PAYLOAD_SIZE ((DEFAULT_PES_HEADER_FREQ - 1) * 184 + 170)

static const AVOption options[] = {
    { "mpegts_transport_stream_id", "Set transport_stream_id field.",
      offsetof(MpegTSWrite, transport_stream_id), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_original_network_id", "Set original_network_id field.",
      offsetof(MpegTSWrite, original_network_id), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_service_id", "Set service_id field.",
      offsetof(MpegTSWrite, service_id), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_final_nb_services", "Set desired number of services.",
      offsetof(MpegTSWrite, final_nb_services), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0004, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_area_code", "Set area_code field.",
      offsetof(MpegTSWrite, area_code), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0DBF, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_guard_interval", "Set guard_interval  field.",
      offsetof(MpegTSWrite, guard_interval), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0004, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_transmission_mode", "Set transmission_mode field.",
      offsetof(MpegTSWrite, transmission_mode), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0004, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_physical_channel", "Set physical_channel field.",
      offsetof(MpegTSWrite, physical_channel), AV_OPT_TYPE_INT, {.i64 = 0x0014 }, 0x000E, 0x0045, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_virtual_channel", "Set virtual_channel field.",
      offsetof(MpegTSWrite, virtual_channel), AV_OPT_TYPE_INT, {.i64 = 0x0014 }, 0x0001, 0x0D45, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_transmission_profile", "Set transmission_profile field.",
      offsetof(MpegTSWrite, transmission_profile), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0002, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_pmt_start_pid", "Set the first pid of the PMT.",
      offsetof(MpegTSWrite, pmt_start_pid), AV_OPT_TYPE_INT, {.i64 = 0x1000 }, 0x0010, 0x1f00, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_start_pid", "Set the first pid.",
      offsetof(MpegTSWrite, start_pid), AV_OPT_TYPE_INT, {.i64 = 0x0100 }, 0x0100, 0x0f00, AV_OPT_FLAG_ENCODING_PARAM},
    {"mpegts_m2ts_mode", "Enable m2ts mode.",
        offsetof(MpegTSWrite, m2ts_mode), AV_OPT_TYPE_INT, {.i64 = -1 },
        -1,1, AV_OPT_FLAG_ENCODING_PARAM},
    { "muxrate", NULL, offsetof(MpegTSWrite, mux_rate), AV_OPT_TYPE_INT, {.i64 = 1}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM},
    { "pes_payload_size", "Minimum PES packet payload in bytes",
      offsetof(MpegTSWrite, pes_payload_size), AV_OPT_TYPE_INT, {.i64 = DEFAULT_PES_PAYLOAD_SIZE}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_flags", "MPEG-TS muxing flags", offsetof(MpegTSWrite, flags), AV_OPT_TYPE_FLAGS, {.i64 = 0}, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    { "resend_headers", "Reemit PAT/PMT before writing the next packet",
      0, AV_OPT_TYPE_CONST, {.i64 = MPEGTS_FLAG_REEMIT_PAT_PMT}, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags"},
    { "latm", "Use LATM packetization for AAC",
      0, AV_OPT_TYPE_CONST, {.i64 = MPEGTS_FLAG_AAC_LATM}, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags"},
    // backward compatibility
    { "resend_headers", "Reemit PAT/PMT before writing the next packet",
      offsetof(MpegTSWrite, reemit_pat_pmt), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_copyts", "don't offset dts/pts",
      offsetof(MpegTSWrite, copyts), AV_OPT_TYPE_INT, {.i64=-1}, -1, 1, AV_OPT_FLAG_ENCODING_PARAM},
    { "tables_version", "set PAT, PMT and SDT version",
      offsetof(MpegTSWrite, tables_version), AV_OPT_TYPE_INT, {.i64=0}, 0, 31, AV_OPT_FLAG_ENCODING_PARAM},
    { NULL },
};

static const AVClass mpegts_muxer_class = {
    .class_name     = "MPEGTS muxer",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};

/* NOTE: 4 bytes must be left at the end for the crc32 */
static void mpegts_write_section(MpegTSSection *s, uint8_t *buf, int len)
{
    unsigned int crc;
    unsigned char packet[TS_PACKET_SIZE];
    const unsigned char *buf_ptr;
    unsigned char *q;
    int first, b, len1, left;

    crc = av_bswap32(av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1, buf, len - 4));
    buf[len - 4] = (crc >> 24) & 0xff;
    buf[len - 3] = (crc >> 16) & 0xff;
    buf[len - 2] = (crc >> 8) & 0xff;
    buf[len - 1] = (crc) & 0xff;

    /* send each packet */
    buf_ptr = buf;
    while (len > 0) {
        first = (buf == buf_ptr);
        q = packet;
        *q++ = 0x47;
        b = (s->pid >> 8);
        if (first)
            b |= 0x40;
        *q++ = b;
        *q++ = s->pid;
        s->cc = (s->cc + 1) & 0xf;
        *q++ = 0x10 | s->cc;
        if (first)
            *q++ = 0; /* 0 offset */
        len1 = TS_PACKET_SIZE - (q - packet);
        if (len1 > len)
            len1 = len;
        memcpy(q, buf_ptr, len1);
        q += len1;
        /* add known padding data */
        left = TS_PACKET_SIZE - (q - packet);
        if (left > 0)
            memset(q, 0xff, left);

        s->write_packet(s, packet);

        buf_ptr += len1;
        len -= len1;
    }
}

static inline void put16(uint8_t **q_ptr, int val)
{
    uint8_t *q;
    q = *q_ptr;
    *q++ = val >> 8;
    *q++ = val;
    *q_ptr = q;
}

static int mpegts_write_section1(MpegTSSection *s, int tid, int id,
                          int version, int sec_num, int last_sec_num,
                          uint8_t *buf, int len)
{
    uint8_t section[1024], *q;
    unsigned int tot_len;
    /* reserved_future_use field must be set to 1 for SDT */
    unsigned int flags = tid == SDT_TID ? 0xf000 : 0xb000;

    tot_len = 3 + 5 + len + 4;
    /* check if not too big */
    if (tot_len > 1024)
        return AVERROR_INVALIDDATA;

    q = section;
    *q++ = tid;
    put16(&q, flags | (len + 5 + 4)); /* 5 byte header + 4 byte CRC */
    put16(&q, id);
    *q++ = 0xc1 | (version << 1); /* current_next_indicator = 1 */
    *q++ = sec_num;
    *q++ = last_sec_num;
    memcpy(q, buf, len);

    mpegts_write_section(s, section, tot_len);
    return 0;
}

/*********************************************/
/* mpegts writer */

#define DEFAULT_PROVIDER_NAME   "FFmpeg"
#define DEFAULT_SERVICE_NAME    "Service01"
#define DEFAULT_NETWORK_NAME    "LaPSI TV - UFRGS"
#define DEFAULT_COUNTRY_CODE    "BRA"

#define DEFAULT_NID		0x0640	// 1600d

/* we retransmit the SI info at this rate */
#define SDT_RETRANS_TIME 500
#define NIT_RETRANS_TIME 50 //Arbitrary value, the brazilian standard requests the NIT to be send every 10 secs.
#define TOT_RETRANS_TIME 100 //Arbitrary value, the brazilian standard requests the NIT to be send every 10 secs.
#define PAT_RETRANS_TIME 100
#define PCR_RETRANS_TIME 20
// TODO Add here the new tables retransmission rate

/* Representa um ES que é enviado através do TS; tem um ponteiro pro serviço correspondente, o PID da stream,
um continuity counter, os valores atuais das PTS e DTS da stream e os dados propriamente ditos */ 

typedef struct MpegTSWriteStream {
    struct MpegTSService *service;
    int pid; /* stream associated pid */
    int cc;
    int payload_size;
    int first_pts_check; ///< first pts check needed
    int prev_payload_key;
    int64_t payload_pts;
    int64_t payload_dts;
    int payload_flags;
    uint8_t *payload;
    AVFormatContext *amux;
} MpegTSWriteStream;

typedef enum {
	GI1_32,
	GI1_16,
	GI1_8,
	GI1_4
} guard_interval_t;

typedef enum {
	MODE1,
	MODE2,
	MODE3,
	UNDEFINED
} transmission_mode_t;

/* NOTE: str == NULL is accepted for an empty string */
static void putstr8(uint8_t **q_ptr, const char *str)
{
    uint8_t *q;
    int len;

    q = *q_ptr;
    if (!str)
        len = 0;
    else
        len = strlen(str);
    *q++ = len;
    memcpy(q, str, len);
    q += len;
    *q_ptr = q;
}

static void mpegts_write_pat(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t data[1012], *q;
    int i;

    q = data;
    for(i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        put16(&q, service->sid);
        put16(&q, 0xe000 | service->pmt.pid);
    }
    mpegts_write_section1(&ts->pat, PAT_TID, ts->tsid, ts->tables_version, 0, 0,
                          data, q - data);
}

static int mpegts_write_pmt(AVFormatContext *s, MpegTSService *service)
{
    MpegTSWrite *ts = s->priv_data;
    uint8_t data[1012], *q, *desc_length_ptr, *program_info_length_ptr, *parental_rating_length_ptr;
    int val, stream_type, i;

    q = data;
    put16(&q, 0xe000 | service->pcr_pid);

    program_info_length_ptr = q;
    q += 2; /* patched after */

	// Parental Rating Descriptor
	*q++ = 0x55; //tag
	parental_rating_length_ptr = q;
	*q++; //length, filled later
    //putstr8(&q, DEFAULT_COUNTRY_CODE);
	//country code with 3 chars, default is BRA
	*q++ = 'B';
	*q++ = 'R';
	*q++ = 'A';
	
	*q++ = 0x01; // RSV 1b | SEX 1b | VIOLENCE 1b | DRUGS 1b | RATING 4b

	//Fill  descriptor length
	parental_rating_length_ptr[0] = q - parental_rating_length_ptr - 1;

    /* put other program info here */

    val = 0xf000 | (q - program_info_length_ptr - 2);
    program_info_length_ptr[0] = val >> 8;
    program_info_length_ptr[1] = val;

    for(i = 0; i < s->nb_streams; i++) {

        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;

	//av_log(s, AV_LOG_VERBOSE, "Stream SID: %d \t Service ID: %d\n", ts_st->service->sid, service->sid);
	if( ts_st->service->sid == service->sid ) {

	//av_log(s, AV_LOG_VERBOSE, "SIDs match, adding this to PMT.\n");
	
	AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL,0);
        switch(st->codec->codec_id) {
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
            stream_type = STREAM_TYPE_VIDEO_MPEG2;
            stream_type = STREAM_TYPE_VIDEO_MPEG2;
            break;
        case AV_CODEC_ID_MPEG4:
            stream_type = STREAM_TYPE_VIDEO_MPEG4;
            break;
        case AV_CODEC_ID_H264:
            stream_type = STREAM_TYPE_VIDEO_H264;
            break;
        case AV_CODEC_ID_HEVC:
            stream_type = STREAM_TYPE_VIDEO_HEVC;
            break;
        case AV_CODEC_ID_CAVS:
            stream_type = STREAM_TYPE_VIDEO_CAVS;
            break;
        case AV_CODEC_ID_DIRAC:
            stream_type = STREAM_TYPE_VIDEO_DIRAC;
            break;
        case AV_CODEC_ID_MP2:
        case AV_CODEC_ID_MP3:
            stream_type = STREAM_TYPE_AUDIO_MPEG1;
            break;
        case AV_CODEC_ID_AAC:
            stream_type = (ts->flags & MPEGTS_FLAG_AAC_LATM) ? STREAM_TYPE_AUDIO_AAC_LATM : STREAM_TYPE_AUDIO_AAC;
            break;
        case AV_CODEC_ID_AAC_LATM:
            stream_type = STREAM_TYPE_AUDIO_AAC_LATM;
            break;
        case AV_CODEC_ID_AC3:
            stream_type = STREAM_TYPE_AUDIO_AC3;
            break;
        default:
            stream_type = STREAM_TYPE_PRIVATE_DATA;
            break;
        }
	//av_log(s, AV_LOG_VERBOSE, "====Stream 0x%x type is 0x%x \n", i, stream_type);

        if (q - data > sizeof(data) - 32)
            return AVERROR(EINVAL);

        *q++ = stream_type;
        put16(&q, 0xe000 | ts_st->pid);
        desc_length_ptr = q;
        q += 2; /* patched after */

        /* write optional descriptors here */
        switch(st->codec->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            if(st->codec->codec_id==AV_CODEC_ID_EAC3){
                *q++=0x7a; // EAC3 descriptor see A038 DVB SI
                *q++=1; // 1 byte, all flags sets to 0
                *q++=0; // omit all fields...
            }
            if( st->codec->codec_id == AV_CODEC_ID_AAC_LATM ){
                *q++=0x7C; // AAC descriptor tag, see ABNT NBR15608
                *q++=0x2; // 2 bytes long, one for profile/level info and another for additional info flag set to zero
                *q++=0x2E; // Profile HE-AACv2, level 4
                *q++=0x00; // MSB is AAC_type_flag, set to zero. Others are reserved.
            }
            if(st->codec->codec_id==AV_CODEC_ID_S302M){
                *q++ = 0x05; /* MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'B';
                *q++ = 'S';
                *q++ = 'S';
                *q++ = 'D';
            }

            if (lang) {
                char *p;
                char *next = lang->value;
                uint8_t *len_ptr;

                *q++ = 0x0a; /* ISO 639 language descriptor */
                len_ptr = q++;
                *len_ptr = 0;

                for (p = lang->value; next && *len_ptr < 255 / 4 * 4 && q - data < sizeof(data) - 4; p = next + 1) {
                    next = strchr(p, ',');
                    if (strlen(p) != 3 && (!next || next != p + 3))
                        continue; /* not a 3-letter code */

                    *q++ = *p++;
                    *q++ = *p++;
                    *q++ = *p++;

                if (st->disposition & AV_DISPOSITION_CLEAN_EFFECTS)
                    *q++ = 0x01;
                else if (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED)
                    *q++ = 0x02;
                else if (st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED)
                    *q++ = 0x03;
                else
                    *q++ = 0; /* undefined type */

                    *len_ptr += 4;
                }

                if (*len_ptr == 0)
                    q -= 2; /* no language codes were written */
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            {
                const char default_language[] = "und";
                const char *language = lang && strlen(lang->value) >= 3 ? lang->value : default_language;

                if (st->codec->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
                    uint8_t *len_ptr;
                    int extradata_copied = 0;

                    *q++ = 0x59; /* subtitling_descriptor */
                    len_ptr = q++;

                    while (strlen(language) >= 3 && (sizeof(data) - (q - data)) >= 8) { /* 8 bytes per DVB subtitle substream data */
                        *q++ = *language++;
                        *q++ = *language++;
                        *q++ = *language++;
                        /* Skip comma */
                        if (*language != '\0')
                            language++;

                        if (st->codec->extradata_size - extradata_copied >= 5) {
                            *q++ = st->codec->extradata[extradata_copied + 4]; /* subtitling_type */
                            memcpy(q, st->codec->extradata + extradata_copied, 4); /* composition_page_id and ancillary_page_id */
                            extradata_copied += 5;
                            q += 4;
                        } else {
                            /* subtitling_type:
                             * 0x10 - normal with no monitor aspect ratio criticality
                             * 0x20 - for the hard of hearing with no monitor aspect ratio criticality */
                            *q++ = (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED) ? 0x20 : 0x10;
                            if ((st->codec->extradata_size == 4) && (extradata_copied == 0)) {
                                /* support of old 4-byte extradata format */
                                memcpy(q, st->codec->extradata, 4); /* composition_page_id and ancillary_page_id */
                                extradata_copied += 4;
                                q += 4;
                            } else {
                                put16(&q, 1); /* composition_page_id */
                                put16(&q, 1); /* ancillary_page_id */
                            }
                        }
                    }

                    *len_ptr = q - len_ptr - 1;
                } else if (st->codec->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
                    uint8_t *len_ptr = NULL;
                    int extradata_copied = 0;

                    /* The descriptor tag. teletext_descriptor */
                    *q++ = 0x56;
                    len_ptr = q++;

                    while (strlen(language) >= 3 && q - data < sizeof(data) - 6) {
                        *q++ = *language++;
                        *q++ = *language++;
                        *q++ = *language++;
                        /* Skip comma */
                        if (*language != '\0')
                            language++;

                        if (st->codec->extradata_size - 1 > extradata_copied) {
                            memcpy(q, st->codec->extradata + extradata_copied, 2);
                            extradata_copied += 2;
                            q += 2;
                        } else {
                            /* The Teletext descriptor:
                             * teletext_type: This 5-bit field indicates the type of Teletext page indicated. (0x01 Initial Teletext page)
                             * teletext_magazine_number: This is a 3-bit field which identifies the magazine number.
                             * teletext_page_number: This is an 8-bit field giving two 4-bit hex digits identifying the page number. */
                            *q++ = 0x08;
                            *q++ = 0x00;
                        }
                    }

                    *len_ptr = q - len_ptr - 1;
                 }
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            if (stream_type == STREAM_TYPE_VIDEO_DIRAC) {
                *q++ = 0x05; /*MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'd';
                *q++ = 'r';
                *q++ = 'a';
                *q++ = 'c';
            }
            break;
        case AVMEDIA_TYPE_DATA:
            if (st->codec->codec_id == AV_CODEC_ID_SMPTE_KLV) {
                *q++ = 0x05; /* MPEG-2 registration descriptor */
                *q++ = 4;
                *q++ = 'K';
                *q++ = 'L';
                *q++ = 'V';
                *q++ = 'A';
            }
            break;
        }

        val = 0xf000 | (q - desc_length_ptr - 2);
        desc_length_ptr[0] = val >> 8;
        desc_length_ptr[1] = val;
    } //if stream service equal current service
    } //for all streams in the context
    mpegts_write_section1(&service->pmt, PMT_TID, service->sid, ts->tables_version, 0, 0,
                          data, q - data);
    return 0;
}


//TODO Add here the other tables: NIT, BAT/SDT


static void mpegts_write_sdt(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t data[1012], *q, *desc_list_len_ptr, *desc_len_ptr;
    int i, running_status, free_ca_mode, val;

    q = data;
    put16(&q, ts->onid);
    *q++ = 0xff;
    for(i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        put16(&q, service->sid);
        *q++ = 0xfc | 0x00; /* currently no EIT info */
        desc_list_len_ptr = q;
        q += 2;
        running_status = 4; /* running */
        free_ca_mode = 0;

        /* write only one descriptor for the service name and provider */
        *q++ = 0x48;
        desc_len_ptr = q;
        q++;
        *q++ = 0x01; /* digital television service */
        putstr8(&q, service->provider_name);
        putstr8(&q, service->name);
        desc_len_ptr[0] = q - desc_len_ptr - 1;

        /* fill descriptor length */
        val = (running_status << 13) | (free_ca_mode << 12) |
            (q - desc_list_len_ptr - 2);
        desc_list_len_ptr[0] = val >> 8;
        desc_list_len_ptr[1] = val;
    }
    mpegts_write_section1(&ts->sdt, SDT_TID, ts->tsid, ts->tables_version, 0, 0,
                          data, q - data);
}

static void mpegts_write_nit(AVFormatContext *s)
{
	MpegTSWrite *ts = s->priv_data;
	uint8_t data[1012], *q, *desc_len_ptr, *ts_loop_len_ptr, *transp_desc_len_ptr;
	uint8_t *ts_info_desc_length_ptr, *service_list_desc_length_ptr, *part_rec_desc_length_ptr, *sys_mgmt_desc_length_ptr, *terr_del_sys_desc_length_ptr;
	int i, temp_val, ts_loop_length_val, transp_desc_len_val;

	q = data;
	
	desc_len_ptr = q;
        q += 2;

	//Network Name Descriptor
	*q++ = 0x40; //tag
        putstr8(&q, DEFAULT_NETWORK_NAME); //length and name string

	// System Management Descriptor
	*q++ = 0xFE; //tag
	sys_mgmt_desc_length_ptr = q;
	*q++; //length, filled later
	*q++ = 0x03; //Bcast flag '00' Open TV, Bcast ID: '000011'
	*q++ = 0x01; //Read from RBS1905.ts

	//Fill  descriptor length
	sys_mgmt_desc_length_ptr[0] = q - sys_mgmt_desc_length_ptr - 1;

	//Other Descriptors
	//...
	//...
	
	//Fill the descriptors length field
	temp_val = 0xF0 << 8 | (q - desc_len_ptr - 2);
	//av_log(s, AV_LOG_VERBOSE, "calculated length: %x %x %d \n", desc_len_ptr[0], desc_len_ptr[1], (q - desc_len_ptr - 2));
	desc_len_ptr[0] = temp_val >> 8;
	desc_len_ptr[1] = temp_val;

	//Begin of TS loop descriptors
	ts_loop_len_ptr = q;
	q +=2;

	//TS ID, 16bits
	put16(&q, ts->tsid);

	//Original Network ID, 16bits
	put16(&q, ts->onid);
	
	//Begin of transport descriptors
	transp_desc_len_ptr = q;
	q +=2;

	//First Descriptor
	//TS Information Descriptor
	*q++ = 0xCD; //tag
	ts_info_desc_length_ptr = q;
	*q++; //length, filled later
	*q++ = ts->virtual_channel; //remote control key id
	//av_log(s, AV_LOG_VERBOSE, "==== virtual channel : %d physical channel %d \n", ts->virtual_channel, ts->physical_channel);
	//length of ts name string, 6 bits | transmission type count, 2 bits
	*q++ = strlen(DEFAULT_NETWORK_NAME) << 2 | 0x2;
	memcpy(q, DEFAULT_NETWORK_NAME, strlen(DEFAULT_NETWORK_NAME));
	q += strlen(DEFAULT_NETWORK_NAME);

	// transmission_profile: variável criada como modo de se alterar/escolher o número de serviços no TS.
	switch (ts->transmission_profile) {
		case 1:
		default:
			for(i = 0; i < ts->nb_services; i++) {
			//	av_log(s, AV_LOG_VERBOSE, "==== service test fields: %x NW_ID:%x SVC_TYPE:%x PGM_NB:%x \n",
			//		ts->services[i]->sid,
			//		(( ts->services[i]->sid & 0xFFE0 ) >> 5 ),
			//		(( ts->services[i]->sid & 0x18 ) >> 3 ),
			//		(ts->services[i]->sid & 0x7 )
			//	);
				if( (ts->services[i]->sid & 0x18 >> 3 )) {//if true, is a 1-seg service
					*q++ = 0xAF; //transmission type: 0xAF: C
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
				else {
					*q++ = 0x0F; //transmission type: 0x0F: A
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
			}
		break;
		case 2:
			
		break;
	}

	//Fill TS info descriptor length
	ts_info_desc_length_ptr[0] = q - ts_info_desc_length_ptr - 1;
	
	//Service List Descriptor
	*q++ = 0x41; //tag
	service_list_desc_length_ptr = q;
	*q++; //length, filled later

	for(i = 0; i < ts->nb_services; i++) {
		put16(&q, ts->services[i]->sid);//service_ID
		*q++ = 0x01; //service type 0x01 for Digital TV Service
	}

	//Fill Service list descriptor length
	service_list_desc_length_ptr[0] = q - service_list_desc_length_ptr - 1;

	for(i = 0; i < ts->nb_services; i++) {
		//av_log(s, AV_LOG_VERBOSE, "==== 1-seg Service test: %x \n", (ts->services[i]->sid & 0x18) >> 3 );
		if(((ts->services[i]->sid & 0x18) >> 3) == 0x3) {//if true, is a 1-seg service
			//av_log(s, AV_LOG_VERBOSE, "==== 1-seg Service detected, creating partial reception desriptor.\n" );
			// Partial Reception Descriptor
			*q++ = 0xFB; //tag
			part_rec_desc_length_ptr = q;
			*q++; //length, filled later
			put16(&q, ts->services[i]->sid);
			//Fill  descriptor length
			part_rec_desc_length_ptr[0] = q - part_rec_desc_length_ptr - 1;
		}
	}

	//// Terrestrial System Delivery Descriptor
	*q++ = 0xFA; //tag
	terr_del_sys_desc_length_ptr = q;
	*q++; //length, filled later
	put16(&q, ts->area_code << 4 | ts->guard_interval << 2 | ts->transmission_mode );// Area code | Guard interval | Transmission mode
	put16(&q,  ( 473 + 6 * ( ts->physical_channel - 14 ) +1/7 ) * 7 );// Frequency field: ( 473 + 6 * ( CH - 14 ) +1/7 ) * 7
	//*q++ = 0x; //

	////Fill  descriptor length
	terr_del_sys_desc_length_ptr[0] = q - terr_del_sys_desc_length_ptr - 1;


	//// Descriptor
	//*q++ = 0x41; //tag
	//_length_ptr = q;
	//*q++; //length, filled later
	//put16(&q, 0x);//
	//*q++ = 0x; //

	////Fill  descriptor length
	//_length_ptr[0] = q - _length_ptr - 1;



	//Fill the Transport descriptors length field first
	transp_desc_len_val = 0xF0 << 8 | (q - transp_desc_len_ptr - 2);

	transp_desc_len_ptr[0] = transp_desc_len_val >> 8;
	transp_desc_len_ptr[1] = transp_desc_len_val;


	//Fill the TS loop length field after, for it contains the Transp. descriptors
	ts_loop_length_val = 0xF0 << 8 | (q - ts_loop_len_ptr - 2);

	ts_loop_len_ptr[0] = ts_loop_length_val >> 8;
	ts_loop_len_ptr[1] = ts_loop_length_val;

	
	
	//Write the table
	mpegts_write_section1(&ts->nit, NIT_TID, ts->onid, ts->tables_version, 0, 0,
                          data, q - data);
}

static void mpegts_write_tot(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t section[1024], *q, *tot_length_ptr, *desc_len_ptr, *offset_desc_length_ptr;
    int i, temp_val;
	unsigned int tot_length;

    q = section;
    *q++ = TOT_TID;
	tot_length_ptr = q;
	q += 2; //Filled later

    *q++ = 0xDD; //UTC-3 byte#0; year
    *q++ = 0xE2; //UTC-3 byte#1; year
    *q++ = 0x10; //UTC-3 byte#2; hour
    *q++ = 0x20; //UTC-3 byte#3; min
    *q++ = 0x30; //UTC-3 byte#4; sec

	//Descriptors...	
	desc_len_ptr = q;
    q += 2;

	//Local Time Offset Descriptor
	*q++ = 0x58; //tag
	offset_desc_length_ptr = q;
	*q++; //length, filled later

	*q++ = 'B'; //
	*q++ = 'R'; //
	*q++ = 'A'; //

	*q++ = 0x03 << 2 | 0x2; //Country Region ID, 6bits | RSV 1bit = '1' | POLARITY 1bit
	put16(&q, 0x0000);// Local Time Offset
	
	//Time of Change
    *q++ = 0xDE; //UTC-3 byte#0; year
    *q++ = 0x7B; //UTC-3 byte#0; year
    *q++ = 0x00; //UTC-3 byte#0; hour
    *q++ = 0x00; //UTC-3 byte#0; min
    *q++ = 0x00; //UTC-3 byte#0; sec

	put16(&q, 0x0100);// Next Time Offset

	//Fill  descriptor length
	offset_desc_length_ptr[0] = q - offset_desc_length_ptr - 1;

	//Fill the descriptors length field
	temp_val = 0xF0 << 8 | (q - desc_len_ptr - 2);
	//av_log(s, AV_LOG_VERBOSE, "calculated length: %x %x %d \n", desc_len_ptr[0], desc_len_ptr[1], (q - desc_len_ptr - 2));
	desc_len_ptr[0] = temp_val >> 8;
	desc_len_ptr[1] = temp_val;



	//Section length field completion
	tot_length = q - tot_length_ptr - 2 + 4;// From beggining of UTC-3 field up to end of CRC: variable (q-ptr-2) + CRC (+4)
    put16(&tot_length_ptr, 0xB000 | tot_length); //number of bytes in the section after the two bytes of section_number

    mpegts_write_section(&ts->tot, section, tot_length + 3); // Add to tot_len the 1byte TID and the 2byte (flags | section_length)
}

static MpegTSService *mpegts_add_service(MpegTSWrite *ts,
                                         int sid,
                                         const char *provider_name,
                                         const char *name)
{
    MpegTSService *service;

    service = av_mallocz(sizeof(MpegTSService));
    if (!service)
        return NULL;
	//Experimental modif to assign 1seg PMT PID according to NBR15608 item 27.4
	//Indirectly, every service SID is modified, but since they never share the same program number, it remains unique
    service->pmt.pid = 0x1FC8 + (sid & 0x03);
    service->sid = sid;
    service->provider_name = av_strdup(provider_name);
    service->name = av_strdup(name);
    service->pcr_pid = 0x1fff;
    dynarray_add(&ts->services, &ts->nb_services, service);
    return service;
}

static int64_t get_pcr(const MpegTSWrite *ts, AVIOContext *pb)
{
    return av_rescale(avio_tell(pb) + 11, 8 * PCR_TIME_BASE, ts->mux_rate) +
           ts->first_pcr;
}

static void mpegts_prefix_m2ts_header(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    if (ts->m2ts_mode) {
        int64_t pcr = get_pcr(s->priv_data, s->pb);
        uint32_t tp_extra_header = pcr % 0x3fffffff;
        tp_extra_header = AV_RB32(&tp_extra_header);
        avio_write(s->pb, (unsigned char *) &tp_extra_header,
                sizeof(tp_extra_header));
    }
}

static void section_write_packet(MpegTSSection *s, const uint8_t *packet)
{
    AVFormatContext *ctx = s->opaque;
    mpegts_prefix_m2ts_header(ctx);
    avio_write(ctx->pb, packet, TS_PACKET_SIZE);
}

static int mpegts_write_header(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st;
    MpegTSService *service;
    AVStream *st, *pcr_st = NULL;
    AVDictionaryEntry *title, *provider;
    int i, j;
    const char *service_name;
    const char *provider_name;
    int *pids;
    int ret;
	int calculated_HD_service_ID, calculated_LD_service_ID;

    if (s->max_delay < 0) /* Not set by the caller */
        s->max_delay = 0;

    // round up to a whole number of TS packets
    ts->pes_payload_size = (ts->pes_payload_size + 14 + 183) / 184 * 184 - 14;

    //ts->tsid = ts->transport_stream_id;
    ts->tsid = ts->original_network_id;
    ts->onid = ts->original_network_id;
    /* allocate a single DVB service */ // Not anymore!

    title = av_dict_get(s->metadata, "service_name", NULL, 0);
    if (!title)
        title = av_dict_get(s->metadata, "title", NULL, 0);
    service_name = title ? title->value : DEFAULT_SERVICE_NAME;
    provider = av_dict_get(s->metadata, "service_provider", NULL, 0);
    provider_name = provider ? provider->value : DEFAULT_PROVIDER_NAME;

	switch (ts->transmission_profile) {

	case 1://One HD service and one LD service
	default:
		//First we calculate the HD service ID based on the network_ID, service type (0x0 for TV, 0x3 for 1-SEG) and program counter
		calculated_HD_service_ID = 0x0000; //Initialization necessary?
		calculated_HD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

	    service = mpegts_add_service(ts, calculated_HD_service_ID, provider_name, service_name);
	    service->pmt.write_packet = section_write_packet;
	    service->pmt.opaque = s;
	    service->pmt.cc = 15;

		calculated_LD_service_ID = 0x0000; //Initialization necessary?
		calculated_LD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x1;

		service = mpegts_add_service(ts, calculated_LD_service_ID, provider_name, service_name);
		service->pmt.write_packet = section_write_packet;
		service->pmt.opaque = s;
		service->pmt.cc = 15;

		ts->final_nb_services = 2;
	break;
	case 2:
		
	break;
}


//    for(i = 0;i < ts->final_nb_services; i++) {
//	    service = mpegts_add_service(ts, ts->service_id+i, provider_name, service_name);
//	
//	    service->pmt.write_packet = section_write_packet;
//	    service->pmt.opaque = s;
//	    service->pmt.cc = 15;
//    }

    //av_log(s, AV_LOG_VERBOSE, "%d services created, expected %d services.\n", ts->nb_services, ts->final_nb_services);

    ts->pat.pid = PAT_PID;
    ts->pat.cc = 15; // Initialize at 15 so that it wraps and be equal to 0 for the first packet we write
    ts->pat.write_packet = section_write_packet;
    ts->pat.opaque = s;

    ts->sdt.pid = SDT_PID;
    ts->sdt.cc = 15;
    ts->sdt.write_packet = section_write_packet;
    ts->sdt.opaque = s;

    ts->nit.pid = NIT_PID;
    ts->nit.cc = 15;
    ts->nit.write_packet = section_write_packet;
    ts->nit.opaque = s;

    ts->tot.pid = TOT_PID;
    ts->tot.cc = 15;
    ts->tot.write_packet = section_write_packet;
    ts->tot.opaque = s;

    pids = av_malloc(s->nb_streams * sizeof(*pids));
    if (!pids)
        return AVERROR(ENOMEM);

    /* assign pids to each stream */
    for(i = 0;i < s->nb_streams; i++) {
        st = s->streams[i];
        avpriv_set_pts_info(st, 33, 1, 90000);
        ts_st = av_mallocz(sizeof(MpegTSWriteStream));
        if (!ts_st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        st->priv_data = ts_st;
        ts_st->payload = av_mallocz(ts->pes_payload_size);
        if (!ts_st->payload) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

	ts_st->service = ts->services[i % ts->final_nb_services] ; //TODO Potential point to modify the stream's owners.

        /* MPEG pid values < 16 are reserved. Applications which set st->id in
         * this range are assigned a calculated pid. */
        if (st->id < 16) {
            ts_st->pid = ts->start_pid + i;
        } else if (st->id < 0x1FFF) {
            ts_st->pid = st->id;
        } else {
            av_log(s, AV_LOG_ERROR, "Invalid stream id %d, must be less than 8191\n", st->id);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (ts_st->pid == ts_st->service->pmt.pid) {
            av_log(s, AV_LOG_ERROR, "Duplicate stream id %d\n", ts_st->pid);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        for (j = 0; j < i; j++)
            if (pids[j] == ts_st->pid) {
                av_log(s, AV_LOG_ERROR, "Duplicate stream id %d\n", ts_st->pid);
                ret = AVERROR(EINVAL);
                goto fail;
            }
        pids[i] = ts_st->pid;
        ts_st->payload_pts = AV_NOPTS_VALUE;
        ts_st->payload_dts = AV_NOPTS_VALUE;
        ts_st->first_pts_check = 1;
        ts_st->cc = 15;
        /* update PCR pid by using the first video stream */
        if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
            ts_st->service->pcr_pid == 0x1fff) {
            ts_st->service->pcr_pid = ts_st->pid;
            pcr_st = st;
        }
        if (st->codec->codec_id == AV_CODEC_ID_AAC &&
            st->codec->extradata_size > 0)
        {
            AVStream *ast;
            ts_st->amux = avformat_alloc_context();
            if (!ts_st->amux) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            ts_st->amux->oformat = av_guess_format((ts->flags & MPEGTS_FLAG_AAC_LATM) ? "latm" : "adts", NULL, NULL);
            if (!ts_st->amux->oformat) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            ast = avformat_new_stream(ts_st->amux, NULL);
            ret = avcodec_copy_context(ast->codec, st->codec);
            if (ret != 0)
                goto fail;
            ret = avformat_write_header(ts_st->amux, NULL);
            if (ret < 0)
                goto fail;
        }
    }

    av_free(pids);

    /* if no video stream, use the first stream as PCR */
    if (ts_st->service->pcr_pid == 0x1fff && s->nb_streams > 0) {
        pcr_st = s->streams[0];
        ts_st = pcr_st->priv_data;
        ts_st->service->pcr_pid = ts_st->pid;
    }

    if (ts->mux_rate > 1) {
        ts_st->service->pcr_packet_period = (ts->mux_rate * PCR_RETRANS_TIME) /
            (TS_PACKET_SIZE * 8 * 1000);
        ts->sdt_packet_period      = (ts->mux_rate * SDT_RETRANS_TIME) /
            (TS_PACKET_SIZE * 8 * 1000);
		ts->nit_packet_period      = (ts->mux_rate * NIT_RETRANS_TIME) /
             (TS_PACKET_SIZE * 8 * 1000);
        ts->pat_packet_period      = (ts->mux_rate * PAT_RETRANS_TIME) /
            (TS_PACKET_SIZE * 8 * 1000);
        ts->tot_packet_period      = (ts->mux_rate * TOT_RETRANS_TIME) /
            (TS_PACKET_SIZE * 8 * 1000);


    //TODO Add something to new tables here.

        if(ts->copyts < 1)
            ts->first_pcr = av_rescale(s->max_delay, PCR_TIME_BASE, AV_TIME_BASE);
    } else {
        /* Arbitrary values, PAT/PMT will also be written on video key frames */
        ts->sdt_packet_period = 200;
        ts->nit_packet_period = 200;
        ts->tot_packet_period = 200;
        ts->pat_packet_period = 40;
	//TODO Add something to new tables here.
        if (pcr_st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!pcr_st->codec->frame_size) {
                av_log(s, AV_LOG_WARNING, "frame size not set\n");
                ts_st->service->pcr_packet_period =
                    pcr_st->codec->sample_rate/(10*512);
            } else {
                ts_st->service->pcr_packet_period =
                    pcr_st->codec->sample_rate/(10*pcr_st->codec->frame_size);
            }
        } else {
            // max delta PCR 0.1s
            ts_st->service->pcr_packet_period =
                pcr_st->codec->time_base.den/(10*pcr_st->codec->time_base.num);
        }
        if(!ts_st->service->pcr_packet_period)
            ts_st->service->pcr_packet_period = 1;
    }

    // output a PCR as soon as possible
    ts_st->service->pcr_packet_count = ts_st->service->pcr_packet_period;
    ts->pat_packet_count = ts->pat_packet_period-1;
    ts->sdt_packet_count = ts->sdt_packet_period-1;
    ts->nit_packet_count = ts->nit_packet_period-1;
    ts->tot_packet_count = ts->tot_packet_period-1;

    if (ts->mux_rate == 1)
        av_log(s, AV_LOG_VERBOSE, "muxrate VBR, ");
    else
        av_log(s, AV_LOG_VERBOSE, "muxrate %d, ", ts->mux_rate);
    av_log(s, AV_LOG_VERBOSE, "pcr every %d pkts, "
           "sdt every %d, nit every %d pkts,"
	   "pat/pmt every %d pkts\n",
           ts_st->service->pcr_packet_period,
           ts->sdt_packet_period,
	   ts->nit_packet_period,
	   ts->pat_packet_period);

    if (ts->m2ts_mode == -1) {
        if (av_match_ext(s->filename, "m2ts")) {
            ts->m2ts_mode = 1;
        } else {
            ts->m2ts_mode = 0;
        }
    }

    avio_flush(s->pb);

    return 0;

 fail:
    av_free(pids);
    for(i = 0;i < s->nb_streams; i++) {
        MpegTSWriteStream *ts_st;
        st = s->streams[i];
        ts_st = st->priv_data;
        if (ts_st) {
            av_freep(&ts_st->payload);
            if (ts_st->amux) {
                avformat_free_context(ts_st->amux);
                ts_st->amux = NULL;
            }
        }
        av_freep(&st->priv_data);
    }
    return ret;
}

/* send SDT, PAT and PMT tables regulary */
static void retransmit_si_info(AVFormatContext *s, int force_pat)
{
    MpegTSWrite *ts = s->priv_data;
    int i;

    if (++ts->sdt_packet_count == ts->sdt_packet_period) {
        ts->sdt_packet_count = 0;
        mpegts_write_sdt(s);
    }
    
    //av_log(s, AV_LOG_VERBOSE, "Entering retransmit si info, nit\n");
    if (++ts->nit_packet_count == ts->nit_packet_period) {
        ts->nit_packet_count = 0;
        mpegts_write_nit(s);
    }

    //av_log(s, AV_LOG_VERBOSE, "Entering retransmit si info, tot\n");
    if (++ts->tot_packet_count == ts->tot_packet_period) {
        ts->tot_packet_count = 0;
        mpegts_write_tot(s);
    }

    if (++ts->pat_packet_count == ts->pat_packet_period || force_pat) {
        ts->pat_packet_count = 0;
        mpegts_write_pat(s);
        for(i = 0; i < ts->nb_services; i++) {
            mpegts_write_pmt(s, ts->services[i]);
        }
    }
}

static int write_pcr_bits(uint8_t *buf, int64_t pcr)
{
    int64_t pcr_low = pcr % 300, pcr_high = pcr / 300;

    *buf++ = pcr_high >> 25;
    *buf++ = pcr_high >> 17;
    *buf++ = pcr_high >> 9;
    *buf++ = pcr_high >> 1;
    *buf++ = pcr_high << 7 | pcr_low >> 8 | 0x7e;
    *buf++ = pcr_low;

    return 6;
}

/* Write a single null transport stream packet */
static void mpegts_insert_null_packet(AVFormatContext *s)
{
    uint8_t *q;
    uint8_t buf[TS_PACKET_SIZE];

    q = buf;
    *q++ = 0x47;
    *q++ = 0x00 | 0x1f;
    *q++ = 0xff;
    *q++ = 0x10;
    memset(q, 0x0FF, TS_PACKET_SIZE - (q - buf));
    mpegts_prefix_m2ts_header(s);
    avio_write(s->pb, buf, TS_PACKET_SIZE);
}

/* Write a single transport stream packet with a PCR and no payload */
static void mpegts_insert_pcr_only(AVFormatContext *s, AVStream *st)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st = st->priv_data;
    uint8_t *q;
    uint8_t buf[TS_PACKET_SIZE];

    q = buf;
    *q++ = 0x47;
    *q++ = ts_st->pid >> 8;
    *q++ = ts_st->pid;
    *q++ = 0x20 | ts_st->cc;   /* Adaptation only */
    /* Continuity Count field does not increment (see 13818-1 section 2.4.3.3) */
    *q++ = TS_PACKET_SIZE - 5; /* Adaptation Field Length */
    *q++ = 0x10;               /* Adaptation flags: PCR present */

    /* PCR coded into 6 bytes */
    q += write_pcr_bits(q, get_pcr(ts, s->pb));

    /* stuffing bytes */
    memset(q, 0xFF, TS_PACKET_SIZE - (q - buf));
    mpegts_prefix_m2ts_header(s);
    avio_write(s->pb, buf, TS_PACKET_SIZE);
}

static void write_pts(uint8_t *q, int fourbits, int64_t pts)
{
    int val;

    val = fourbits << 4 | (((pts >> 30) & 0x07) << 1) | 1;
    *q++ = val;
    val = (((pts >> 15) & 0x7fff) << 1) | 1;
    *q++ = val >> 8;
    *q++ = val;
    val = (((pts) & 0x7fff) << 1) | 1;
    *q++ = val >> 8;
    *q++ = val;
}

/* Set an adaptation field flag in an MPEG-TS packet*/
static void set_af_flag(uint8_t *pkt, int flag)
{
    // expect at least one flag to set
    av_assert0(flag);

    if ((pkt[3] & 0x20) == 0) {
        // no AF yet, set adaptation field flag
        pkt[3] |= 0x20;
        // 1 byte length, no flags
        pkt[4] = 1;
        pkt[5] = 0;
    }
    pkt[5] |= flag;
}

/* Extend the adaptation field by size bytes */
static void extend_af(uint8_t *pkt, int size)
{
    // expect already existing adaptation field
    av_assert0(pkt[3] & 0x20);
    pkt[4] += size;
}

/* Get a pointer to MPEG-TS payload (right after TS packet header) */
static uint8_t *get_ts_payload_start(uint8_t *pkt)
{
    if (pkt[3] & 0x20)
        return pkt + 5 + pkt[4];
    else
        return pkt + 4;
}

/* Add a pes header to the front of payload, and segment into an integer number of
 * ts packets. The final ts packet is padded using an over-sized adaptation header
 * to exactly fill the last ts packet.
 * NOTE: 'payload' contains a complete PES payload.
 */
static void mpegts_write_pes(AVFormatContext *s, AVStream *st,
                             const uint8_t *payload, int payload_size,
                             int64_t pts, int64_t dts, int key)
{
    MpegTSWriteStream *ts_st = st->priv_data;
    MpegTSWrite *ts = s->priv_data;
    uint8_t buf[TS_PACKET_SIZE];
    uint8_t *q;
    int val, is_start, len, header_len, write_pcr, is_dvb_subtitle, is_dvb_teletext, flags;
    int afc_len, stuffing_len;
    int64_t pcr = -1; /* avoid warning */
    int64_t delay = av_rescale(s->max_delay, 90000, AV_TIME_BASE);
    int force_pat = st->codec->codec_type == AVMEDIA_TYPE_VIDEO && key && !ts_st->prev_payload_key;

    is_start = 1;
    while (payload_size > 0) {
        retransmit_si_info(s, force_pat);
        force_pat = 0;

        write_pcr = 0;
        if (ts_st->pid == ts_st->service->pcr_pid) {
            if (ts->mux_rate > 1 || is_start) // VBR pcr period is based on frames
                ts_st->service->pcr_packet_count++;
            if (ts_st->service->pcr_packet_count >=
                ts_st->service->pcr_packet_period) {
                ts_st->service->pcr_packet_count = 0;
                write_pcr = 1;
            }
        }

        if (ts->mux_rate > 1 && dts != AV_NOPTS_VALUE &&
            (dts - get_pcr(ts, s->pb)/300) > delay) {
            /* pcr insert gets priority over null packet insert */
            if (write_pcr)
                mpegts_insert_pcr_only(s, st);
            else
                mpegts_insert_null_packet(s);
            continue; /* recalculate write_pcr and possibly retransmit si_info */
        }

        /* prepare packet header */
        q = buf;
        *q++ = 0x47;
        val = (ts_st->pid >> 8);
        if (is_start)
            val |= 0x40;
        *q++ = val;
        *q++ = ts_st->pid;
        ts_st->cc = (ts_st->cc + 1) & 0xf;
        *q++ = 0x10 | ts_st->cc; // payload indicator + CC
        if (key && is_start && pts != AV_NOPTS_VALUE) {
            // set Random Access for key frames
            if (ts_st->pid == ts_st->service->pcr_pid)
                write_pcr = 1;
            set_af_flag(buf, 0x40);
            q = get_ts_payload_start(buf);
        }
        if (write_pcr) {
            set_af_flag(buf, 0x10);
            q = get_ts_payload_start(buf);
            // add 11, pcr references the last byte of program clock reference base
            if (ts->mux_rate > 1)
                pcr = get_pcr(ts, s->pb);
            else
                pcr = (dts - delay)*300;
            if (dts != AV_NOPTS_VALUE && dts < pcr / 300)
                av_log(s, AV_LOG_WARNING, "dts < pcr, TS is invalid\n");
            extend_af(buf, write_pcr_bits(q, pcr));
            q = get_ts_payload_start(buf);
        }
        if (is_start) {
            int pes_extension = 0;
            int pes_header_stuffing_bytes = 0;
            /* write PES header */
            *q++ = 0x00;
            *q++ = 0x00;
            *q++ = 0x01;
            is_dvb_subtitle = 0;
            is_dvb_teletext = 0;
            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (st->codec->codec_id == AV_CODEC_ID_DIRAC) {
                    *q++ = 0xfd;
                } else
                    *q++ = 0xe0;
            } else if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
                       (st->codec->codec_id == AV_CODEC_ID_MP2 ||
                        st->codec->codec_id == AV_CODEC_ID_MP3 ||
                        st->codec->codec_id == AV_CODEC_ID_AAC)) {
                *q++ = 0xc0;
            } else if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
                        st->codec->codec_id == AV_CODEC_ID_AC3 &&
                        ts->m2ts_mode) {
                *q++ = 0xfd;
            } else {
                *q++ = 0xbd;
                if(st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    if (st->codec->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
                        is_dvb_subtitle = 1;
                    } else if (st->codec->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
                        is_dvb_teletext = 1;
                    }
                }
            }
            header_len = 0;
            flags = 0;
            if (pts != AV_NOPTS_VALUE) {
                header_len += 5;
                flags |= 0x80;
            }
            if (dts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && dts != pts) {
                header_len += 5;
                flags |= 0x40;
            }
            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
                st->codec->codec_id == AV_CODEC_ID_DIRAC) {
                /* set PES_extension_flag */
                pes_extension = 1;
                flags |= 0x01;

                /*
                * One byte for PES2 extension flag +
                * one byte for extension length +
                * one byte for extension id
                */
                header_len += 3;
            }
            /* for Blu-ray AC3 Audio the PES Extension flag should be as follow
             * otherwise it will not play sound on blu-ray
             */
            if (ts->m2ts_mode &&
                st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
                st->codec->codec_id == AV_CODEC_ID_AC3) {
                        /* set PES_extension_flag */
                        pes_extension = 1;
                        flags |= 0x01;
                        header_len += 3;
            }
            if (is_dvb_teletext) {
                pes_header_stuffing_bytes = 0x24 - header_len;
                header_len = 0x24;
            }
            len = payload_size + header_len + 3;
            /* 3 extra bytes should be added to DVB subtitle payload: 0x20 0x00 at the beginning and trailing 0xff */
            if (is_dvb_subtitle) {
                len += 3;
                payload_size++;
            }
            if (len > 0xffff)
                len = 0;
            *q++ = len >> 8;
            *q++ = len;
            val = 0x80;
            /* data alignment indicator is required for subtitle and data streams */
            if (st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE || st->codec->codec_type == AVMEDIA_TYPE_DATA)
                val |= 0x04;
            *q++ = val;
            *q++ = flags;
            *q++ = header_len;
            if (pts != AV_NOPTS_VALUE) {
                write_pts(q, flags >> 6, pts);
                q += 5;
            }
            if (dts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && dts != pts) {
                write_pts(q, 1, dts);
                q += 5;
            }
            if (pes_extension && st->codec->codec_id == AV_CODEC_ID_DIRAC) {
                flags = 0x01;  /* set PES_extension_flag_2 */
                *q++ = flags;
                *q++ = 0x80 | 0x01;  /* marker bit + extension length */
                /*
                * Set the stream id extension flag bit to 0 and
                * write the extended stream id
                */
                *q++ = 0x00 | 0x60;
            }
            /* For Blu-ray AC3 Audio Setting extended flags */
          if (ts->m2ts_mode &&
              pes_extension &&
              st->codec->codec_id == AV_CODEC_ID_AC3) {
                      flags = 0x01; /* set PES_extension_flag_2 */
                      *q++ = flags;
                      *q++ = 0x80 | 0x01; /* marker bit + extension length */
                      *q++ = 0x00 | 0x71; /* for AC3 Audio (specifically on blue-rays) */
              }


            if (is_dvb_subtitle) {
                /* First two fields of DVB subtitles PES data:
                 * data_identifier: for DVB subtitle streams shall be coded with the value 0x20
                 * subtitle_stream_id: for DVB subtitle stream shall be identified by the value 0x00 */
                *q++ = 0x20;
                *q++ = 0x00;
            }
            if (is_dvb_teletext) {
                memset(q, 0xff, pes_header_stuffing_bytes);
                q += pes_header_stuffing_bytes;
            }
            is_start = 0;
        }
        /* header size */
        header_len = q - buf;
        /* data len */
        len = TS_PACKET_SIZE - header_len;
        if (len > payload_size)
            len = payload_size;
        stuffing_len = TS_PACKET_SIZE - header_len - len;
        if (stuffing_len > 0) {
            /* add stuffing with AFC */
            if (buf[3] & 0x20) {
                /* stuffing already present: increase its size */
                afc_len = buf[4] + 1;
                memmove(buf + 4 + afc_len + stuffing_len,
                        buf + 4 + afc_len,
                        header_len - (4 + afc_len));
                buf[4] += stuffing_len;
                memset(buf + 4 + afc_len, 0xff, stuffing_len);
            } else {
                /* add stuffing */
                memmove(buf + 4 + stuffing_len, buf + 4, header_len - 4);
                buf[3] |= 0x20;
                buf[4] = stuffing_len - 1;
                if (stuffing_len >= 2) {
                    buf[5] = 0x00;
                    memset(buf + 6, 0xff, stuffing_len - 2);
                }
            }
        }

        if (is_dvb_subtitle && payload_size == len) {
            memcpy(buf + TS_PACKET_SIZE - len, payload, len - 1);
            buf[TS_PACKET_SIZE - 1] = 0xff; /* end_of_PES_data_field_marker: an 8-bit field with fixed contents 0xff for DVB subtitle */
        } else {
            memcpy(buf + TS_PACKET_SIZE - len, payload, len);
        }

        payload += len;
        payload_size -= len;
        mpegts_prefix_m2ts_header(s);
        avio_write(s->pb, buf, TS_PACKET_SIZE);
    }
    avio_flush(s->pb);
    ts_st->prev_payload_key = key;
}

int ff_check_h264_startcode(AVFormatContext *s, const AVStream *st, const AVPacket *pkt)
{
    if (pkt->size < 5 || AV_RB32(pkt->data) != 0x0000001) {
        if (!st->nb_frames) {
            av_log(s, AV_LOG_ERROR, "H.264 bitstream malformed, "
                   "no startcode found, use the h264_mp4toannexb bitstream filter (-bsf h264_mp4toannexb)\n");
            return AVERROR(EINVAL);
        }
        av_log(s, AV_LOG_WARNING, "H.264 bitstream error, startcode missing\n");
    }
    return 0;
}

static int mpegts_write_packet_internal(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    int size = pkt->size;
    uint8_t *buf= pkt->data;
    uint8_t *data= NULL;
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st = st->priv_data;
    const int64_t delay = av_rescale(s->max_delay, 90000, AV_TIME_BASE)*2;
    int64_t dts = pkt->dts, pts = pkt->pts;

    if (ts->reemit_pat_pmt) {
        av_log(s, AV_LOG_WARNING, "resend_headers option is deprecated, use -mpegts_flags resend_headers\n");
        ts->reemit_pat_pmt = 0;
        ts->flags |= MPEGTS_FLAG_REEMIT_PAT_PMT;
    }

    if (ts->flags & MPEGTS_FLAG_REEMIT_PAT_PMT) {
        ts->pat_packet_count = ts->pat_packet_period - 1;
        ts->sdt_packet_count = ts->sdt_packet_period - 1;
        ts->nit_packet_count = ts->nit_packet_period - 1;
        ts->tot_packet_count = ts->tot_packet_period - 1;
        ts->flags &= ~MPEGTS_FLAG_REEMIT_PAT_PMT;
    }

    if(ts->copyts < 1){
        if (pts != AV_NOPTS_VALUE)
            pts += delay;
        if (dts != AV_NOPTS_VALUE)
            dts += delay;
    }

    if (ts_st->first_pts_check && pts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_ERROR, "first pts value must be set\n");
        return AVERROR_INVALIDDATA;
    }
    ts_st->first_pts_check = 0;

    if (st->codec->codec_id == AV_CODEC_ID_H264) {
        const uint8_t *p = buf, *buf_end = p+size;
        uint32_t state = -1;
        int ret = ff_check_h264_startcode(s, st, pkt);
        if (ret < 0)
            return ret;

        do {
            p = avpriv_find_start_code(p, buf_end, &state);
            av_dlog(s, "nal %d\n", state & 0x1f);
        } while (p < buf_end && (state & 0x1f) != 9 &&
                 (state & 0x1f) != 5 && (state & 0x1f) != 1);

        if ((state & 0x1f) != 9) { // AUD NAL
            data = av_malloc(pkt->size+6);
            if (!data)
                return AVERROR(ENOMEM);
            memcpy(data+6, pkt->data, pkt->size);
            AV_WB32(data, 0x00000001);
            data[4] = 0x09;
            data[5] = 0xf0; // any slice type (0xe) + rbsp stop one bit
            buf  = data;
            size = pkt->size+6;
        }
    } else if (st->codec->codec_id == AV_CODEC_ID_AAC) {
        if (pkt->size < 2) {
            av_log(s, AV_LOG_ERROR, "AAC packet too short\n");
            return AVERROR_INVALIDDATA;
        }
        if ((AV_RB16(pkt->data) & 0xfff0) != 0xfff0) {
            int ret;
            AVPacket pkt2;

            if (!ts_st->amux) {
                av_log(s, AV_LOG_ERROR, "AAC bitstream not in ADTS format "
                       "and extradata missing\n");
                return AVERROR_INVALIDDATA;
            }

            av_init_packet(&pkt2);
            pkt2.data = pkt->data;
            pkt2.size = pkt->size;
            ret = avio_open_dyn_buf(&ts_st->amux->pb);
            if (ret < 0)
                return AVERROR(ENOMEM);

            ret = av_write_frame(ts_st->amux, &pkt2);
            if (ret < 0) {
                avio_close_dyn_buf(ts_st->amux->pb, &data);
                ts_st->amux->pb = NULL;
                av_free(data);
                return ret;
            }
            size = avio_close_dyn_buf(ts_st->amux->pb, &data);
            ts_st->amux->pb = NULL;
            buf = data;
        }
    }

    if (pkt->dts != AV_NOPTS_VALUE) {
        int i;
        for(i=0; i<s->nb_streams; i++){
            AVStream *st2 = s->streams[i];
            MpegTSWriteStream *ts_st2 = st2->priv_data;
            if(   ts_st2->payload_size
               && (ts_st2->payload_dts == AV_NOPTS_VALUE || dts - ts_st2->payload_dts > delay/2)){
                mpegts_write_pes(s, st2, ts_st2->payload, ts_st2->payload_size,
                                ts_st2->payload_pts, ts_st2->payload_dts,
                                ts_st2->payload_flags & AV_PKT_FLAG_KEY);
                ts_st2->payload_size = 0;
            }
        }
    }

    if (ts_st->payload_size && ts_st->payload_size + size > ts->pes_payload_size) {
        mpegts_write_pes(s, st, ts_st->payload, ts_st->payload_size,
                         ts_st->payload_pts, ts_st->payload_dts,
                         ts_st->payload_flags & AV_PKT_FLAG_KEY);
        ts_st->payload_size = 0;
    }

    if (st->codec->codec_type != AVMEDIA_TYPE_AUDIO || size > ts->pes_payload_size) {
        av_assert0(!ts_st->payload_size);
        // for video and subtitle, write a single pes packet
        mpegts_write_pes(s, st, buf, size, pts, dts, pkt->flags & AV_PKT_FLAG_KEY);
        av_free(data);
        return 0;
    }

    if (!ts_st->payload_size) {
        ts_st->payload_pts = pts;
        ts_st->payload_dts = dts;
        ts_st->payload_flags = pkt->flags;
    }

    memcpy(ts_st->payload + ts_st->payload_size, buf, size);
    ts_st->payload_size += size;

    av_free(data);

    return 0;
}

static void mpegts_write_flush(AVFormatContext *s)
{
    int i;

    /* flush current packets */
    for(i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;
        if (ts_st->payload_size > 0) {
            mpegts_write_pes(s, st, ts_st->payload, ts_st->payload_size,
                             ts_st->payload_pts, ts_st->payload_dts,
                             ts_st->payload_flags & AV_PKT_FLAG_KEY);
            ts_st->payload_size = 0;
        }
    }
    avio_flush(s->pb);
}

static int mpegts_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    if (!pkt) {
        mpegts_write_flush(s);
        return 1;
    } else {
        return mpegts_write_packet_internal(s, pkt);
    }
}

static int mpegts_write_end(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    int i;

    mpegts_write_flush(s);

    for(i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;
        av_freep(&ts_st->payload);
        if (ts_st->amux) {
            avformat_free_context(ts_st->amux);
            ts_st->amux = NULL;
        }
    }

    for(i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        av_freep(&service->provider_name);
        av_freep(&service->name);
        av_free(service);
    }
    av_free(ts->services);

    return 0;
}

AVOutputFormat ff_mpegts_muxer = {
    .name              = "mpegts",
    .long_name         = NULL_IF_CONFIG_SMALL("MPEG-TS (MPEG-2 Transport Stream)"),
    .mime_type         = "video/x-mpegts",
    .extensions        = "ts,m2t,m2ts,mts",
    .priv_data_size    = sizeof(MpegTSWrite),
    .audio_codec       = AV_CODEC_ID_MP2,
    .video_codec       = AV_CODEC_ID_MPEG2VIDEO,
    .write_header      = mpegts_write_header,
    .write_packet      = mpegts_write_packet,
    .write_trailer     = mpegts_write_end,
    .flags             = AVFMT_ALLOW_FLUSH,
    .priv_class        = &mpegts_muxer_class,
};
