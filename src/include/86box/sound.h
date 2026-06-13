/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Sound emulation core.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Jasmine Iwanek, <jriwanek@gmail.com>
 *
 *          Copyright 2008-2018 Sarah Walker.
 *          Copyright 2016-2025 Miran Grca.
 *          Copyright 2024-2026 Jasmine Iwanek.
 */
#ifndef EMU_SOUND_H
#define EMU_SOUND_H

#define SOUND_CARD_MAX 4 /* currently we support up to 4 sound cards and a standalone MPU401 */

extern int  sound_gain;
extern char sound_output_device[512]; /* selected audio output device name, empty = system default */

#define FREQ_44100  44100
#define FREQ_48000  48000
#define FREQ_48558  48558
#define FREQ_49716  49716
#define FREQ_55930  55930
#define FREQ_88200  88200
#define FREQ_96000  96000

#define SOUND_FREQ   FREQ_48000
#define SOUNDBUFLEN  SOUND_FREQ

#define MUSIC_FREQ   FREQ_49716
#define MUSICBUFLEN  MUSIC_FREQ

#define YM2151_FREQ  FREQ_55930
#define YM2151BUFLEN YM2151_FREQ

#define CQM_FREQ     FREQ_48558
#define CQMBUFLEN    CQM_FREQ

#define CD_FREQ      FREQ_44100
#define CD_BUFLEN    (CD_FREQ / 10)

#define WT_FREQ      FREQ_44100
#define WTBUFLEN     WT_FREQ

enum {
    SOUND_NONE = 0,
    SOUND_INTERNAL
};

enum {
    SOUND_U8 = 0,
    SOUND_S16,
    SOUND_FLOAT32,
    SOUND_MULAW,
    SOUND_ALAW,
    SOUND_IMA_ADPCM,
    SOUND_MAX
};

typedef union {
    uint8_t *u8;
    int16_t *s16;
    int32_t *s32;
    int64_t *s64;
    float   *f;
} sound_buffer_t;

extern int ppispeakon;
extern int gated;
extern int speakval;
extern int speakon;

extern int sound_card_current[SOUND_CARD_MAX];

#define sound_add_handler(get_buffer, priv)     sound_add_legacy_source((get_buffer), (priv), FREQ_48000, #get_buffer)
#define music_add_handler(get_buffer, priv)     sound_add_legacy_source((get_buffer), (priv), FREQ_49716, #get_buffer)
#define ym2151_add_handler(get_buffer, priv)    sound_add_legacy_source((get_buffer), (priv), FREQ_55930, #get_buffer)
#define cqm_add_handler(get_buffer, priv)       sound_add_legacy_source((get_buffer), (priv), FREQ_48558, #get_buffer)
#define wavetable_add_handler(get_buffer, priv) sound_add_legacy_source((get_buffer), (priv), FREQ_44100, #get_buffer)
extern void *sound_add_legacy_source(void (*get_buffer)(int32_t *buffer,
                                                        uint16_t len, void *priv),
                                     void *priv, uint32_t freq, const char *name);
extern int   sound_get_legacy_pos(void *priv);

extern void sound_set_cd_audio_filter(void (*filter)(int     channel,
                                                     double *buffer, void *priv),
                                      void *priv);
extern void sound_set_pc_speaker_filter(void (*filter)(int     channel,
                                                       double *buffer, void *priv),
                                        void *priv);
extern void sound_set_midi_filter(void (*filter)(int     channel,
                                                 double *buffer, void *priv),
                                  void *priv);

extern void (*filter_pc_speaker)(int channel, double *buffer, void *priv);
extern void *filter_pc_speaker_p;

extern void (*filter_midi)(int channel, double *buffer, void *priv);
extern void *filter_midi_p;

extern int sound_card_available(int card);
#ifdef EMU_DEVICE_H
extern const device_t *sound_card_getdevice(int card);
#endif
extern int         sound_card_has_config(int card);
extern const char *sound_card_get_internal_name(int card);
extern int         sound_card_get_from_internal_name(const char *s);
extern void        sound_card_init(void);
extern void        sound_set_cd_volume(unsigned int vol_l, unsigned int vol_r);

extern void sound_speed_changed(void);

extern void  sound_init(void);
extern void  sound_reset(void);
extern void *sound_add_source(uint8_t (*poll)(sound_buffer_t buffer, void *priv), void *priv, const char *name);
extern void  sound_start_source(void *priv);
extern void  sound_set_format(void *priv, uint8_t format, uint8_t channels, uint32_t freq);
extern uint32_t sound_get_freq(void *priv);

extern void sound_card_reset(void);

extern void sound_cd_thread_end(void);
extern void sound_cd_thread_reset(void);

extern void sound_fdd_thread_init(void);
extern void sound_fdd_thread_end(void);

extern void sound_hdd_thread_init(void);
extern void sound_hdd_thread_end(void);

extern const char *sound_get_output_devices(void); /* returns double-null-terminated list, or NULL */

extern void  sound_backend_reset(void);
extern void *sound_backend_add_source(void);
extern void  sound_backend_stop_source(void *priv);
extern int   sound_backend_set_format(void *priv, uint8_t *format, uint8_t *channels, uint32_t *freq);
extern void  sound_backend_buffer(void *priv, void *buf, uint32_t bytes);

extern const int16_t sound_mulaw_table[256];
extern const int16_t sound_alaw_table[256];
#define sound_convert_u8(sample)    (int16_t) (((sample) ^ 0x80) << 8)
#define sound_convert_f32(sample)   (((sample) >= 1.0f) ? 32767 : (((sample) <= -1.0f) ? -32768 : (int16_t) ((sample) * 32767.0f)))
#define sound_convert_mulaw(sample) (sound_mulaw_table[(sample) & ((sizeof(sound_mulaw_table) / sizeof(sound_mulaw_table[0])) - 1)])
#define sound_convert_alaw(sample)  (sound_alaw_table[(sample) & ((sizeof(sound_alaw_table) / sizeof(sound_alaw_table[0])) - 1)])

#define sb_vibra16c_onboard_relocate_base sb_vibra16s_onboard_relocate_base
#define sb_vibra16cl_onboard_relocate_base sb_vibra16s_onboard_relocate_base
#define sb_vibra16xv_onboard_relocate_base sb_vibra16s_onboard_relocate_base
extern void sb_vibra16s_onboard_relocate_base(uint16_t new_addr, void *priv);

#ifdef EMU_DEVICE_H
/* AdLib and AdLib Gold */
extern const device_t adlib_device;
extern const device_t adlib_mca_device;
extern const device_t adgold_device;

/* Analog Devices AD1816 */
extern const device_t ad1816_device;

/* Aztech Sound Galaxy 16 */
extern const device_t azt2316a_device;
extern const device_t azt1605_device;
extern const device_t aztpr16_device;
extern const device_t azt2316r_device;
extern const device_t azt2320_device;

/* C-Media CMI8x38 */
extern const device_t cmi8338_device;
extern const device_t cmi8338_onboard_device;
extern const device_t cmi8738_device;
extern const device_t cmi8738_onboard_device;
extern const device_t cmi8738_6ch_onboard_device;

/* Covox ISA */
extern const device_t voicemasterkey_device;
extern const device_t soundmaster_device;
extern const device_t soundmasterplus_device;
extern const device_t isadacr0_device;
extern const device_t isadacr1_device;

/* Creative Labs Game Blaster */
extern const device_t cms_device;

/* Creative Labs Sound Blaster */
extern const device_t sb_1_device;
extern const device_t sb_15_device;
extern const device_t sb_mcv_device;
extern const device_t sb_2_device;
extern const device_t sb_pro_v1_device;
extern const device_t sb_pro_v2_device;
extern const device_t sb_pro_mcv_device;
extern const device_t sb_pro_compat_device;
extern const device_t sb_16_device;
extern const device_t sb_vibra16c_onboard_device;
extern const device_t sb_vibra16c_device;
extern const device_t sb_vibra16cl_onboard_device;
extern const device_t sb_vibra16cl_device;
extern const device_t sb_vibra16s_onboard_device;
extern const device_t sb_vibra16s_device;
extern const device_t sb_vibra16xv_onboard_device;
extern const device_t sb_vibra16xv_device;
extern const device_t sb_16_pnp_device;
extern const device_t sb_16_pnp_ide_device;
extern const device_t sb_16_compat_device;
extern const device_t sb_16_compat_nompu_device;
extern const device_t sb_16_reply_mca_device;
extern const device_t sb_goldfinch_device;
extern const device_t sb_32_pnp_device;
extern const device_t sb_awe32_device;
extern const device_t sb_awe32_pnp_device;
extern const device_t sb_awe32_ide_pnp_device;
extern const device_t sb_awe64_value_device;
extern const device_t sb_awe64_device;
extern const device_t sb_awe64_ide_device;
extern const device_t sb_awe64_gold_device;

/* Crystal CS423x */
extern const device_t cs4232_device;
extern const device_t cs4232_onboard_device;
extern const device_t cs4235_device;
extern const device_t cs4235_onboard_device;
extern const device_t cs4236_onboard_device;
extern const device_t cs4236b_device;
extern const device_t cs4236b_onboard_device;
extern const device_t cs4237b_device;
extern const device_t cs4238b_device;

/* ESS Technology */
extern const device_t ess_688_device;
extern const device_t ess_ess0100_pnp_device;
extern const device_t ess_ess0968_pnp_688_device;
extern const device_t ess_1688_device;
extern const device_t ess_1688_compaq_device;
extern const device_t ess_ess0102_pnp_device;
extern const device_t ess_ess0968_pnp_device;
extern const device_t ess_soundpiper_16_mca_device;
extern const device_t ess_soundpiper_32_mca_device;
extern const device_t ess_chipchat_16_mca_device;
extern const device_t ess_1788_device;
extern const device_t ess_1888_device;
extern const device_t ess_1888_compaq_device;
extern const device_t ess_1887_device;

/* Ensoniq AudioPCI */
extern const device_t es1370_device;
extern const device_t es1371_device;
extern const device_t es1371_onboard_device;
extern const device_t es1373_device;
extern const device_t es1373_onboard_device;
extern const device_t ct5880_device;
extern const device_t ct5880_onboard_device;

/* Gravis UltraSound family */
extern const device_t gus_device;
extern const device_t gus_v37_device;
extern const device_t gus_max_device;
extern const device_t gus_ace_device;

/* IBM Music Feature Card */
extern const device_t imfc_device;

/* IBM PS/1 Audio Card */
extern const device_t ps1snd_device;

/* Innovation SSI-2001 */
extern const device_t ssi2001_device;
extern const device_t entertainer_device;

/* Mindscape Music Board */
extern const device_t mmb_device;

/* MediaVision ThunderBoard */
extern const device_t thunderboard_device;

/* OPTi 82c93x */
extern const device_t acermagic_s20_device;
extern const device_t mirosound_pcm10_device;
extern const device_t opti_82c930_device;
extern const device_t opti_82c931_device;

/* Pro Audio Spectrum, Plus, 16, and 16D */
extern const device_t pas_device;
extern const device_t pasplus_device;
extern const device_t pas16_device;
extern const device_t pas16d_device;

/* Rainbow Arts PC-Soundman */
extern const device_t soundman_device;

/* Tandy PSSJ */
extern const device_t pssj_device;
extern const device_t pssj_isa_device;
extern const device_t pssj_1e0_device;

/* Tandy PSG */
extern const device_t tndy_device;

/* Tandy Sensation */
extern const device_t sensationaud_device;

/* TexElec SAAYM */
extern const device_t saaym_device;

/* Windows Sound System */
extern const device_t wss_device;
extern const device_t ncr_business_audio_device;

/* Yamaha YMF-7xx */
extern const device_t ymf701_device;
extern const device_t ymf715_onboard_device;
extern const device_t ymf718_device;
extern const device_t ymf719_device;

#ifdef USE_LIBSERIALPORT
/* External Audio device OPL2Board (Host Connected hardware)*/
extern const device_t opl2board_device;
#endif 

#endif

#endif /*EMU_SOUND_H*/
