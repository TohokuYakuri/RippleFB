#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

/* #define ANALOG_OUTPUT_DEBUG */

#ifndef BOOL
#define BOOL int
#endif

#ifndef TRUE
#define TRUE 1 ///< define true for C
#endif

#ifndef FALSE
#define FALSE 0 ///< define false for C
#endif

#include "../ExtensionCommon/nspPlugin.h"
#include "../ExtensionCommon/nspChanTrigPlugin.h"

#define REQSAMPLES 10
#define BUFSIZE 300
#define ENVELOPE_EVAL_BUFSIZE 600 /* 30*20 20ms */
#define ENVELOPE_G_BUFSIZE 300    /* 20*15 */
#define ENVELOPE_G_STEP 0.013   /* 0.2/15 */
#define ENVELOPE_E_STEP 0.08    /* 1.2/15 */
#define INITIAL_MEAN 130.0
#define INITIAL_SIGMA 90.0
#define INITIAL_THRESH_SD 4.0
#define N_SMOOTH 150000.0       /* 10000*15 */
#define MIN_STIM_INT 250000       /* 250,000us; 250ms */
//#define MIN_STIM_INT 500000       /* 250,000us; 250ms */
#define MASK_SIGNAL_THRESH 8000
#define DELAY_CTRL_LEN 250000   // us

#define COMID_PROCESS_ENABLE 128
#define COMID_SET_SIG_CH 127
#define COMID_SET_REF_CH 126
#define COMID_SET_MASK_CH 125
#define COMID_SET_THRESH_SD 124
#define COMID_UPDATE_PARAMS 123 /* update mean and sd */
#define COMID_CHMODE_MASK 122
#define COMID_CHMODE_CONTROL 121
#define COMID_CHMODE_REF 120
#define COMID_SHOW_SETTINGS 119


typedef struct
{
    double g_buf[ENVELOPE_G_BUFSIZE];
    int g_idx;
    double mean_est;
    double sigma_est;
    double x;
    double g;
    double envelope_val;
    double envelope_eval_buf[ENVELOPE_EVAL_BUFSIZE];
    double mean_eval_env;
} envelope_t;

 /* global variables */
float v_diff[BUFSIZE] = {0};
float v_bp[BUFSIZE] = {0};
float dataBuffer[3][BUFSIZE] = {{0}}; // This buffer contains the obtained data for all channels
envelope_t g_env;
uint16_t g_nChan = CBEXT_FRONTEND_COUNT+1;
uint16_t g_nRefChan = CBEXT_FRONTEND_COUNT+2;
uint16_t g_nMaskChan = CBEXT_FRONTEND_COUNT+3;
float g_thresh_sd = INITIAL_THRESH_SD;
double g_mean_val = INITIAL_MEAN;
double g_sd_val = INITIAL_SIGMA;
double g_thresh_val = INITIAL_MEAN + INITIAL_SIGMA * INITIAL_THRESH_SD;
volatile BOOL g_is_process_enable = FALSE; /* if TRUE, feedback is enable */
volatile BOOL g_is_trigger_enable = TRUE;
volatile BOOL g_is_control_mode = FALSE;
volatile BOOL g_is_ref_enable = FALSE;
volatile BOOL g_is_mask_mode = FALSE;
volatile BOOL g_is_masking_val = FALSE; /* TRUE if mask_signal > MASK_SIGNAL_THRESH */
uint16_t g_nDOutTrigChan = 0;   ///< Digital output channel to trigger. default: 0
uint16_t g_nTriggerIndex = 0;   ///< Trigger waveform index
uint16_t g_nAOutTrigChan = 0;   ///< Analog output channel to trigger
/* bandpass filter params */
const double Filt_a[] = { 1.0, -3.90776515, 5.7306583, -3.73786603, 0.91497583 };
const double Filt_b[] = { 0.00094469, 0.0, -0.00188938, 0.0, 0.00094469 };
const int Filt_order = 2;


/* function prototype */
static void bandpass_filter(float * vin, float * vout, uint32_t n_samples);
static double calc_mean_for_g_buf(envelope_t * envelope_buf);
static void calc_envelope(envelope_t * env, float * v_bp, uint32_t retrievedSamples);
static double calc_mean_for_envelope(envelope_t * envelope_buf);
static void ResetBuffers(void);
static int ProcessComment(cbExtComment * pComment);

cbExtResult cbExtMainLoop(cbExtSettings * settings)
{
    printf("\nRipple FB loaded\n");
    cbExtResult res = CBEXTRESULT_SUCCESS;
    /* data buffer */
    float tempDataBuffer[CBEXT_INPUT_COUNT][REQSAMPLES] = {{0}}; // This buffer contains the obtained data for all channels
    ResetBuffers();
    /* other variables */
    uint32_t retrievedSamples = 0;
    struct timeval before_time;
    gettimeofday(&before_time, NULL);
    struct timeval current_time;
    long delta_t = 0;
    uint64_t time_from_stim = 0;
    int i = 0;

    /* Set up sample buffer */
    cbExtChanTrigSamples samples;
    memset(&samples, 0, sizeof(samples));
    samples.isCount.nCountFrontend = REQSAMPLES;
    samples.isCount.nCountAnalogInput = REQSAMPLES;
    for (i=0; i<CBEXT_INPUT_COUNT; i++)
    {
        if (i < CBEXT_FRONTEND_COUNT)
            samples.isFrontend.pfData[i] = &tempDataBuffer[i][0];
        else
            samples.isAnalogInput.pfData[i-CBEXT_FRONTEND_COUNT] = &tempDataBuffer[i][0];
    }

    /* analog output settings */
    cbExtChanTrigOutputSamples SampleArray;
    memset(&SampleArray, 0, sizeof(SampleArray));
    uint32_t outproctime = 0;
    res = cbExtGetCurrentSampleTime(&outproctime);
    SampleArray.isAnalogOutput.pnProctime = &outproctime;
    int16_t anaoutp[CBEXT_ANALOGOUTPUT_COUNT][REQSAMPLES];
    memset(anaoutp, 0, sizeof(anaoutp));
    for (i=0; i<CBEXT_ANALOGOUTPUT_COUNT; i++)
    {
        SampleArray.isAnalogOutput.pnData[i] = &anaoutp[i][0];
    }
    SampleArray.isCount.nCountAnalogOutput = REQSAMPLES;


    /* for process comment */
    cbExtComment cmt;
    memset(&cmt, 0, sizeof(cmt));

    while (res != CBEXTRESULT_EXIT)
    {
        /* process comment */
        res = cbExtGetOneComment(&cmt);
        if (res == CBEXTRESULT_EXIT)
            break;
        if (res == CBEXTRESULT_SUCCESS)
        {
            ProcessComment(&cmt);
        }

        /* Reset requested num */
        samples.isCount.nCountFrontend = REQSAMPLES;
        samples.isCount.nCountAnalogInput = REQSAMPLES;

        /* Get data */
        res = cbExtChanTrigGetSamples(&samples);
        if (res == CBEXTRESULT_EXIT)
                break;

        if (res == CBEXTRESULT_SUCCESS)
        {
            retrievedSamples = samples.isCount.nCountFrontend;
            /* retrievedSamples = samples.isCount.nCountAnalogInput; */
            if (retrievedSamples) {
                /* get time */
                gettimeofday(&current_time, NULL);
                delta_t = current_time.tv_usec - before_time.tv_usec;
                time_from_stim += delta_t;
                before_time.tv_usec = current_time.tv_usec;

                /* move data */
                memmove(&dataBuffer[0][retrievedSamples], &dataBuffer[0][0],(BUFSIZE-retrievedSamples)*sizeof(float));
                memmove(&dataBuffer[1][retrievedSamples], &dataBuffer[1][0],(BUFSIZE-retrievedSamples)*sizeof(float));
                memmove(&dataBuffer[2][retrievedSamples], &dataBuffer[2][0],(BUFSIZE-retrievedSamples)*sizeof(float));
                memmove(&v_diff[retrievedSamples], &v_diff[0], (BUFSIZE-retrievedSamples)*sizeof(float));
                memmove(&v_bp[retrievedSamples], &v_bp[0], (BUFSIZE-retrievedSamples)*sizeof(float));
                /* copy data */
                memcpy(&dataBuffer[0][0], &tempDataBuffer[g_nChan-1][0],(retrievedSamples)*sizeof(float));
                memcpy(&dataBuffer[1][0], &tempDataBuffer[g_nRefChan-1][0],(retrievedSamples)*sizeof(float));
                memcpy(&dataBuffer[2][0], &tempDataBuffer[g_nMaskChan-1][0],(retrievedSamples)*sizeof(float));

                /* process data */
                for (i = 0; i < retrievedSamples; i++) {
                    v_diff[retrievedSamples-i-1] = dataBuffer[0][i] - g_is_ref_enable * dataBuffer[1][i];
                    g_is_masking_val = (dataBuffer[2][i] > MASK_SIGNAL_THRESH) ? TRUE : FALSE;
                }
                bandpass_filter(v_diff, v_bp, retrievedSamples);
                calc_envelope(&g_env, v_bp, retrievedSamples);

                /* feedback */
                if (g_is_process_enable && g_is_trigger_enable) {
                    if (g_env.mean_eval_env > g_thresh_val && ((g_is_mask_mode && g_is_masking_val) || (!g_is_mask_mode))) {
                        time_from_stim = 0;
                        g_is_trigger_enable = FALSE;
                        res = g_is_control_mode 
                            ? cbExtChanTrigAnalogOutput(g_nAOutTrigChan, g_nTriggerIndex) 
                            : cbExtChanTrigDigitalOutput(g_nDOutTrigChan, g_nTriggerIndex);
                        printf("val: %5.1f, mean: %5.1f, sd: %5.1f\n", g_env.mean_eval_env, g_env.mean_est, g_env.sigma_est);
                    }
                } else if (time_from_stim > MIN_STIM_INT) {
                    g_is_trigger_enable = TRUE;
                }

                #ifdef ANALOG_OUTPUT_DEBUG
                /* analog output */
                memset(anaoutp, 0, sizeof(anaoutp));
                res = cbExtGetCurrentSampleTime(&outproctime);
                for (i = 0; i < retrievedSamples; i++) {
                    // anaoutp[0][i] = (int16_t)dataBuffer[0][i];
                    /* anaoutp[0][i] = (int16_t)v_bp[retrievedSamples - i - 1]; */
                    anaoutp[0][i] = (int16_t)g_env.envelope_eval_buf[retrievedSamples-i-1];
                }
                SampleArray.isCount.nCountAnalogOutput = retrievedSamples;
                res = cbExtChanTrigSendSamples(&SampleArray, FALSE);
                #endif
            }
        }
    } // end while (res != CBEXTRESULT_EXIT
    /* sprintf(logMsg, "Goodbye!\n"); */
    /* cbExtLogEvent(logMsg); */
    return CBEXTRESULT_SUCCESS;
}


cbExtResult cbExtSetup(cbExtInfo * info)
{
    // The pieces in info that are not set here will have 0 as default, which disables them
    info->nPluginVer = 1; // Give this a version
    info->nWarnCommentsThreshold = 90; // Warn on 90% buffer
    strncpy(info->szName, "HelloWorld", sizeof(info->szName));
    cbExtCommentMask iMask;
    iMask.nCharsetMask = 0x90;    // Interested in charsets of 128 and 16 (0x80 + 0x10)
    iMask.flags = CBEXT_CMT_NONE; // also want normal comments, but am not interested in NeuroMotive events
    info->iMask = iMask;

    return CBEXTRESULT_SUCCESS;
}

cbExtResult cbExtChanTrigSetup(cbExtChanTrigInfo * info)
{
    // Having this function defiend is equivalant of chantrig intent
    // The pieces in info that are not set here will have 0 as default, which disables them

    info->nDividerAnalogInput = 1; // Need full sample-rate
    info->nDividerFrontend = 1; // Need full sample-rate
    return CBEXTRESULT_SUCCESS;
}

static void bandpass_filter(float * vin, float * vout, uint32_t n_samples)
{
    long j, k;
    double bfiltval = 0;
    double afiltval = 0;
    for (k = n_samples - 1; k >= 0; --k) {
        bfiltval = 0;
        afiltval = 0;
        for (j = 0; j<2 * Filt_order + 1; ++j) {
            bfiltval = bfiltval + Filt_b[j] * vin[k + j];
        }
        for (j = 1; j<2 * Filt_order + 1; ++j) {
            afiltval = afiltval + Filt_a[j] * vout[k + j];
        }
        vout[k] = bfiltval - afiltval;
    }
}

static double calc_mean_for_g_buf(envelope_t * envelope_buf)
{
    int i;
    double res = 0;
    for (i=0; i<ENVELOPE_G_BUFSIZE; ++i) {
        res += envelope_buf->g_buf[i];
    }
    return (res/ENVELOPE_G_BUFSIZE);
}

static void calc_envelope(envelope_t * env, float * v_bp, uint32_t retrievedSamples)
{
    memmove(&env->envelope_eval_buf[retrievedSamples], &env->envelope_eval_buf[0], (ENVELOPE_EVAL_BUFSIZE-retrievedSamples)*sizeof(double));
    int i;
    for (i=retrievedSamples-1; i>=0; --i) {
        env->x = (v_bp[i] > 0) ? v_bp[i] : - v_bp[i];
        env->sigma_est = (env->x > env->mean_est)
            ? ((env->x - env->mean_est) - env->sigma_est)/N_SMOOTH + env->sigma_est
            : ((env->mean_est - env->x) - env->sigma_est)/N_SMOOTH + env->sigma_est;
        env->mean_est = env->mean_est * (N_SMOOTH - 1) / N_SMOOTH + env->x / N_SMOOTH;
        if (env->x <= env->envelope_val) {
            env->g_buf[env->g_idx] = ENVELOPE_G_STEP;
            env->g = ENVELOPE_G_STEP;
        } else {
            env->g_buf[env->g_idx] = ENVELOPE_E_STEP;
            env->g = calc_mean_for_g_buf(env);
        }
        env->envelope_val = env->envelope_val + env->g * (env->x - env->envelope_val);
        env->envelope_eval_buf[i] = env->envelope_val;
        env->g_idx++;
        if (env->g_idx >= ENVELOPE_G_BUFSIZE) {
            env->g_idx = 0;
        }
    }
    env->mean_eval_env = calc_mean_for_envelope(env);
}

static double calc_mean_for_envelope(envelope_t * envelope_buf)
{
    int i;
    double res = 0;
    for (i=0; i<ENVELOPE_EVAL_BUFSIZE; ++i) {
        res += envelope_buf->envelope_eval_buf[i];
    }
    return (res/ENVELOPE_EVAL_BUFSIZE);
}

static void ResetBuffers(void)
{
    memset(v_diff, 0, sizeof(v_diff));
    memset(v_bp, 0, sizeof(v_bp));
    memset(dataBuffer, 0, sizeof(dataBuffer));

    /* envelope settings */
    int i;
    for (i=0; i<ENVELOPE_G_BUFSIZE; i++) {
        g_env.g_buf[i] = ENVELOPE_G_STEP;
    }
    g_env.g_idx = 0;
    g_env.mean_est = INITIAL_MEAN;
    g_env.sigma_est = INITIAL_SIGMA;
    g_env.x = 0;
    g_env.g = ENVELOPE_G_STEP;
    g_env.envelope_val = 0;
    g_env.mean_eval_env = 0;
    memset(g_env.envelope_eval_buf, 0, ENVELOPE_EVAL_BUFSIZE*sizeof(double));

    printf("\nBuffer reset\n");
}

static int ProcessComment(cbExtComment * pComment)
{
    // See if comment comes from Central and parse it
    if (pComment->nCharset == 0) {
        printf("comments: %s\n", pComment->szCmt);
        size_t stringlen = strlen(pComment->szCmt);
        if (stringlen > 0) {
            const char delimiters[] = " .,;:!-";
            char *token;
            token = strtok (pComment->szCmt, delimiters);
            if (!strcmp("plugin", token)) {
                token = strtok (NULL, delimiters);
                if (token == NULL) return -1;
                pComment->nCharset = atoi(token);
                token = strtok (NULL, delimiters);
                if (token == NULL) return -1;
                pComment->nData = atoi(token);
            }
        }

        /* printf("nchar %d, ndata %d\n", pComment->nCharset, pComment->nData); */
        /* char logMsg[128]; */
        /* memset(logMsg, 0, sizeof(logMsg)); */
        /* sprintf(logMsg, "nchar %d, ndata %d\n", pComment->nCharset, pComment->nData); */
        /* cbExtLogEvent(logMsg); */
    }

    char logMsg[128];
    memset(logMsg, 0, sizeof(logMsg));
    if (pComment->nCharset == COMID_PROCESS_ENABLE) {
        g_is_process_enable = pComment->nData;
        sprintf(logMsg, "Set g_is_process_enable %d\n", g_is_process_enable);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_SET_SIG_CH) {
        g_nChan = pComment->nData;
        sprintf(logMsg, "Set g_nChan %d\n", g_nChan);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_SET_REF_CH) {
        g_nRefChan = pComment->nData;
        sprintf(logMsg, "Set g_nRefChan %d\n", g_nRefChan);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_SET_MASK_CH) {
        g_nMaskChan = pComment->nData;
        sprintf(logMsg, "Set g_nMaskChan %d\n", g_nMaskChan);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_SET_THRESH_SD) {
        /* g_thresh_sd = pComment->nData; */
        g_thresh_sd = (float)((pComment->nData & 0xFFFF0000)>>16) + 0.0001*(float)(pComment->nData & 0x0000FFFF);
        sprintf(logMsg, "Set g_thresh_sd %.2f\n", g_thresh_sd);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_UPDATE_PARAMS) {
        g_mean_val = g_env.mean_est;
        g_sd_val = g_env.sigma_est;
        g_thresh_val = g_mean_val + g_thresh_sd * g_sd_val;
        sprintf(logMsg, "Set g_mean_val %.2f, g_sd_val %.2f\n", g_mean_val, g_sd_val);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_CHMODE_MASK) {
        g_is_mask_mode = pComment->nData;
        sprintf(logMsg, "Set g_is_mask_mode %d\n", g_is_mask_mode);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_CHMODE_CONTROL) {
        g_is_control_mode = pComment->nData;
        sprintf(logMsg, "Set g_is_control_mode %d\n", g_is_control_mode);
        cbExtLogEvent(logMsg);
    } else if (pComment->nCharset == COMID_CHMODE_REF) {
        g_is_ref_enable = pComment->nData;
        sprintf(logMsg, "Set g_is_ref_enable %d\n", g_is_ref_enable);
        cbExtLogEvent(logMsg);
    }
    else if (pComment->nCharset == COMID_SHOW_SETTINGS) {
        printf("Set g_is_process_enable %d\n", g_is_process_enable);
        printf("Set g_nChan %d\n", g_nChan);
        printf("Set g_nRefChan %d\n", g_nRefChan);
        printf("Set g_nMaskChan %d\n", g_nMaskChan);
        printf("Set g_thresh_sd %.2f\n", g_thresh_sd);
        printf("Set g_mean_val %.2f, g_sd_val %.2f\n", g_mean_val, g_sd_val);
        printf("Set g_is_mask_mode %d\n", g_is_mask_mode);
        printf("Set g_is_control_mode %d\n", g_is_control_mode);
        printf("Set g_is_ref_enable %d\n", g_is_ref_enable);
    }

    return 0;
}
