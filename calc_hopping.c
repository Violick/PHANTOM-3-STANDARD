#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "rf_common.h"
#include "bk5811_demodu.h"

void sigint_callback_handler(int signum);
int rx_callback(hackrf_transfer *transfer);

// my
int scan_signal_channel(uint64_t freq_hz);

/*
 *  period  : 112ms
 *  sample number per period : pp = 112 * DEFAULT_SAMPLE_RATE_HZ * 2 / 1000
 *  total : mm = pp * 16  
 *
 */
#define TIMES_PER_CHANNEL       2
#define TOTAL_CHANNELS          16
#define NUMBER_PER_PERIOD_ONE_CHANNEL       (112 * DEFAULT_SAMPLE_RATE_HZ * 2 / 1000 * TIMES_PER_CHANNEL)
#define NUMBER_PER_PERIOD_ALL_CHANNELS      (NUMBER_PER_PERIOD_ONE_CHANNEL * TOTAL_CHANNELS)

// debug info
#define IN_DEBUG     1   
#define OUT_FUNCTION 1
#define IN_FUNCTION  0
#define REAL_TIME_DATA 0
#define LOCAL_DATA   1

static hackrf_device* device = NULL;
static bool do_exit = false;
static int do_per_channel = 0;
char *rx_buffer = NULL;
static size_t rx_length = 0;
int8_t channels[TOTAL_CHANNELS] = {13,18,23,28,33,38,43,48,53,58,63,68,73,78,83,88};
// time : ms
float cost_times[TOTAL_CHANNELS] = {0.0};
int start, end;
int cost = 0;
int8_t g_ord[16] = {0};
static packet_param pp = INIT_PP();
static rf_param rp = RF_PARAM_INIT();
decode_param *dp = NULL;
s_packet *sp = NULL;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[])
{
    int exit_code = EXIT_SUCCESS;

    rp.path = "data/1M_ALL_CHANNEL_ONE_PERIOD.iq"; 
    
    char* file_name = rp.path;
    if (argc == 2)
        file_name = argv[1];

#if REAL_TIME_DATA    
    int result;
    int8_t i;
#if OUT_FUNCTION && IN_DEBUG
    fprintf(stdout, "watch !!! out function.\n");
#endif
#if IN_FUNCTION && IN_DEBUG
    fprintf(stdout, "watch !!! in function.\n");
#endif

    signal(SIGINT, &sigint_callback_handler); 
    signal(SIGILL, &sigint_callback_handler); 
    signal(SIGFPE, &sigint_callback_handler); 
    signal(SIGSEGV, &sigint_callback_handler); 
    signal(SIGTERM, &sigint_callback_handler); 
    signal(SIGABRT, &sigint_callback_handler); 
    
    rp.baseband_filter_bw_hz = hackrf_compute_baseband_filter_bw(rp.baseband_filter_bw_hz);
    
    result = hackrf_init();
    if(result != HACKRF_SUCCESS)
        return -1;
    
    uint64_t freq_hz = rp.freq_hz;

    // set rx_buffer
    rx_buffer = (char *)malloc(NUMBER_PER_PERIOD_ALL_CHANNELS);
    if(NULL == rx_buffer)
    {
        fprintf(stderr, "alloc memory failed.\n");
        exit(0);
    }
    memset(rx_buffer, 0, NUMBER_PER_PERIOD_ALL_CHANNELS);
   
    for(i = 0; i < TOTAL_CHANNELS; i++)
    {
        freq_hz = START_FREQ +  (channels[i] * FREQ_ONE_MHZ);
        //fprintf(stderr, "channel number : %d\t channel frequency : %llu\n", channels[i], freq_hz);
#if OUT_FUNCTION
        if( 0 == cost)
            start = clock();
#endif
        scan_signal_channel(freq_hz);
#if OUT_FUNCTION
        end = clock();
        cost_times[cost++] = (end - start) * 1000.0 / CLOCKS_PER_SEC;
        start = end;
#endif
        if ( do_exit )
            break;
    }
#if (OUT_FUNCTION || IN_FUNCTION) && IN_DEBUG
    for(i = 0; i < TOTAL_CHANNELS; i++)
        fprintf(stdout, "cost times %f at %d channel.\n", cost_times[i]/TIMES_PER_CHANNEL, channels[i]);
    //exit(0);
#endif

    // it should be in a loop if could't get the ord.
    //pthread_mutex_lock(&mutex);
    //
    //pthread_mutex_unlock(&mutex);

    // write local file
    FILE *fd = NULL;
    fd = fopen(file_name, "wb");
    if(NULL != fd)
    {
        fwrite(rx_buffer, 1, rx_length, fd);
        fclose(fd);
    }
    fprintf(stdout, "Write signal in \"%s\" file.\n ", file_name);
#endif

#if LOCAL_DATA
    // read local file.
    long file_length = get_file_size(file_name);
    long read_length = 0;
    rx_buffer = (char *)malloc(file_length);
    get_signal_data(file_name, rx_buffer, 0, &read_length);
    rx_length = file_length;
    fprintf(stdout, "Read signal from \"%s\" file.\n ", file_name);
#endif

    dp = (decode_param *)malloc(sizeof(decode_param));
    sp = (s_packet *)malloc(sizeof(s_packet));

    float times = 0;
    long first_position = 0;
    long last_position = 0;
    uint8_t l_channel = 0;
    int time_slot = 0;
    float threshold = 0.0;
    int split = TIMES_PER_CHANNEL;
    long begin = 0;
    long step = NUMBER_PER_PERIOD_ONE_CHANNEL / split;

    for(int i = 0; i < TOTAL_CHANNELS * split; i++ )
    {
        memset(dp, 0, sizeof(decode_param));
        memset(sp, 0, sizeof(s_packet));
        begin = i * NUMBER_PER_PERIOD_ONE_CHANNEL / split;
        threshold = mean(rx_buffer, begin, step, dp); 
        find_inter(rx_buffer, begin, step, dp); 

        if( 1 == work(rx_buffer, dp, &pp, sp))
        {
            //  first signal position
            if(first_position == 0)
            {
                first_position = last_position;
            }
            times = (last_position - first_position) * 1000.0 / (DEFAULT_SAMPLE_RATE_HZ * 2 * 7) + 0.5;
            time_slot = (int)(times) % TOTAL_CHANNELS;
            float a = last_position * 1000.0 / (DEFAULT_SAMPLE_RATE_HZ * 2 * 7) + 0.5;
            printf("channel : %d<-->%d\tg_threshold : %f\ttime_slot : %f<-->%d\ttime : %f<-->%d\n", channels[i/split], l_channel, threshold, times, time_slot, a, (int)(a)%16); 
            g_ord[time_slot] = channels[i/split];
        }
    }

    fprintf(stdout, "\n");
    for(int i = 0; i < TOTAL_CHANNELS; i++)
        fprintf(stdout, "%d,", g_ord[i]);
    fprintf(stdout, "\n");

    free(rx_buffer);
    free(dp);
    free(sp);
    rx_buffer = NULL;
    hackrf_exit();

    return exit_code;
}

int scan_signal_channel(uint64_t freq_hz)
{
    int result;
    do_per_channel = 0;

    result = hackrf_open(&device);
    if( result != HACKRF_SUCCESS)
    {
        fprintf(stderr, "can not open hackrf.\nexit.");
        exit(0);
    }
    result = hackrf_set_sample_rate_manual(device, rp.sample_rate_hz, 1);
    result = hackrf_set_baseband_filter_bandwidth(device, rp.baseband_filter_bw_hz);
    result = hackrf_set_vga_gain(device, rp.vga_gain);
    result |= hackrf_set_lna_gain(device, rp.lna_gain);
    result |= hackrf_start_rx(device, rx_callback, NULL);
    result = hackrf_set_freq(device, freq_hz);
    result = hackrf_set_amp_enable(device, (uint8_t)rp.amp_enable);
#if IN_FUNCTION
    if(0 == cost)
        start = clock();
#endif
    while( (hackrf_is_streaming(device) == HACKRF_TRUE) && (do_per_channel < NUMBER_PER_PERIOD_ONE_CHANNEL) );
#if IN_FUNCtION
    end = clock();
    cost_times[cost++] = (end - start) * 1000.0 / CLOCKS_PER_SEC;
    start = end;
#endif
    

    result = hackrf_stop_rx(device); 
    result = hackrf_close(device);
    return 0;
}

void sigint_callback_handler(int signum)
{
    fprintf(stdout, "Caught signal %d\n", signum);
    do_exit = true;
}

int rx_callback(hackrf_transfer *transfer)
{
    //pthread_mutex_lock(&mutex);
    for(int i = 0; i < transfer->buffer_length; i += 2)
    {
       if(do_per_channel < NUMBER_PER_PERIOD_ONE_CHANNEL)
       {
            rx_buffer[rx_length++] = transfer->buffer[i]; 
            rx_buffer[rx_length++] = transfer->buffer[i + 1];
            do_per_channel += 2;
       }
       else
           break;
    }
    //pthread_mutex_unlock(&mutex);
    return 0;
}
