#include "syn.h"
#include "bsp_pwm.h"
#include "stdlib.h"
#include "tim.h"
#include "bsp_usart.h"
#include "usart.h"
#include "bsp_log.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

static void* syn_id = (void*)10086;

static USARTInstance* gprmc_usart;
static PWMInstance* lidar_pps;
static PWMInstance* cam_trigger;

static uint8_t utc_hh_idx = 0, utc_mm_idx = 0, utc_ss_idx = 0;  
// CUHK(SZ) to youth!
static char gprmc[] = "$GPRMC,xxxxxx.00,A,2241.289544,N,11412.769625,E,0.0,225.5,040519,2.3,W,A*23\n\0";
static int len;

// trigger in rising edge
// 目前最大支持一天的包?
 void PPSCbk()
{
    // calc RTC
    if(++utc_ss_idx == 60)
    {
        utc_ss_idx = 0;
        if(++utc_mm_idx == 60)
        {
            utc_mm_idx = 0;
            if(++utc_hh_idx == 24)
            {
                utc_hh_idx = 0;
                LOGERROR("[hard syn]: exceed maximum time");
                LOGERROR("[hard syn]: we will stop here");
            }
        }
    }
    // update gprmc
    sprintf(gprmc+7,"%02d",utc_hh_idx);
    sprintf(gprmc+9,"%02d",utc_mm_idx);
    sprintf(gprmc+11,"%02d",utc_ss_idx);
    gprmc[13]='.'; // sprintf will add '\0' in the end, replace it with '.'
    
    USARTSend(gprmc_usart,(uint8_t*) gprmc,len,USART_TRANSFER_DMA);
}

void InitHardSyn()
{
    USART_Init_Config_s usart_config={
        .usart_handle = &huart1,
        .module_callback = NULL,
        .recv_buff_size = 128
    };
    gprmc_usart = USARTRegister(&usart_config);

    PWM_Init_Config_s pps_config={
        .htim = &htim1,
        .channel = TIM_CHANNEL_1,
        .period = 1000, // 1000ms
        .dutyratio = 10, // 10%
        .id = syn_id,
        .callback = NULL
    };
    lidar_pps = PWMRegister(&pps_config);

    PWM_Init_Config_s cam_config={
        .htim = &htim8,
        .channel = TIM_CHANNEL_1,
        .period = 100, // 100ms,adjust this to change the trigger frequency
        .dutyratio = 20, //50%, some camera may have a requirement on the pulse width, adjust this parameter
        .id = syn_id,
        .callback = NULL
    };
    cam_trigger = PWMRegister(&cam_config);

    len = strlen(gprmc); // calc len

    // there will be a little error between the two PWMs, but it's ok
    PWMStart(lidar_pps);
    PWMStart(cam_trigger);
    HAL_TIM_Base_Start_IT(&htim1); // start timer
}
