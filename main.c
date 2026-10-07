/******************************************************************************
 * File Name: main.c
 *
 * Description: Three-phase complementary PWM generation with dead time, and
 *              simultaneous three-channel SAR ADC sampling triggered from a
 *              dedicated TCPWM counter at the centre of every PWM period.
 *
 * Related Document: See README.md
 *
 ******************************************************************************
 * $ Copyright 2026-YEAR Infineon Technologies AG $
 *****************************************************************************/

/******************************************************************************
 * Header Files
 *****************************************************************************/
#include "cybsp.h"
#include "cy_retarget_io.h"

/******************************************************************************
 * Macros
 *****************************************************************************/
/* Number of ADC channels */
#define ADC_NUM_OF_CHANNELS         (3u)

/* Assign the ADC interrupt number and priority */
#define ADC_IRQ_NUM                 (NvicMux2_IRQn)
#define ADC0_INTR_CH0_NUM            (((ADC_IRQ_NUM << 16u) | ADC0_CH_0_IRQ))
#define ADC1_INTR_CH0_NUM            (((ADC_IRQ_NUM << 16u) | ADC1_CH_0_IRQ))
#define ADC2_INTR_CH0_NUM            (((ADC_IRQ_NUM << 16u) | ADC2_CH_0_IRQ))
#define ADC_INTR_PRIORITY           (0u)

/* TCPWM trigger input to start PWM simultaneously
 * All the TCPWM inputs are the same so it's fine if it's
 * PWM_V_start_0_TRIGGER_OUT, PWM_W_start_0_TRIGGER_OUT or PWM_TRG_ADC_start_0_TRIGGER_OUT
 */
#define TCPWM_ALL_TRIG_INPUT        (PWM_U_start_0_TRIGGER_OUT)

/******************************************************************************
 * Global Variables
 *****************************************************************************/
/* ADC interrupt configuration */
const cy_stc_sysint_t IRQ_CFG_ADC0 =
{
    .intrSrc = ADC0_INTR_CH0_NUM,
    .intrPriority = ADC_INTR_PRIORITY,
};
const cy_stc_sysint_t IRQ_CFG_ADC1 =
{
    .intrSrc = ADC1_INTR_CH0_NUM,
    .intrPriority = ADC_INTR_PRIORITY,
};
const cy_stc_sysint_t IRQ_CFG_ADC2 =
{
    .intrSrc = ADC2_INTR_CH0_NUM,
    .intrPriority = ADC_INTR_PRIORITY,
};

/* ADC channel result */
static uint16_t g_adcResultBuffer[ADC_NUM_OF_CHANNELS] = {0};

/* ADC channel EoS interrupt flag */
static bool g_flagSarGroupEoS = false;

/* Used for retarget-io (debug UART) */
static cy_stc_scb_uart_context_t    UART_context;
static mtb_hal_uart_t               UART_hal_obj;

/******************************************************************************
 * Function Prototypes
 *****************************************************************************/
/* Function to handle the ADC callback events */
static void HandleAdcGrpInt(void);

/* Function to enable the UART terminal output for debugging */
static void EnableTerminalOutput(void);

/* Function to get the ADC conversion result for one channel */
static uint32_t GetAdcResult(PASS_SAR_Type *base, uint32_t ch);

/* TCPWM Trigger setting function to update duty ratio when the period ends */
static void UpdateDutyRatio(void);

/* Multi-channel SAR initialization function */
static void InitAdcMultiChannel(void);

/* PWM channel initialization function */
static void InitPwmAll(void);

/* Trigger ADC conversion */
static void StartConversion(void);

/******************************************************************************
 * Function Definitions
 *****************************************************************************/
/******************************************************************************
 * Function Name: HandleError
 ******************************************************************************
 * Summary:
 *  User-defined error handling function.
 *
 * Parameters:
 *  status - status for evaluation.
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void HandleError(cy_rslt_t status)
{
    if (CY_RSLT_SUCCESS != status)
    {
        /* Halt the CPU while debugging */
        printf("Initialization failed\r\n");
        CY_ASSERT(0);
    }
}

/******************************************************************************
 * Function Name: main
 ******************************************************************************
 * Summary:
 *  This is the main function.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  int
 *
 *****************************************************************************/
int main(void)
{
    /* API return code */
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();
    HandleError(result);

    /* Enable global interrupts */
    __enable_irq();

    EnableTerminalOutput();

    /* \x1b[2J\x1b[;H - ANSI ESC sequence to clear the screen */
    printf("\x1b[2J\x1b[;H");
    printf("********************************************************************************\r\n");
    printf(" PDL: Three-phase PWM and synchronized three-channel ADC sampling \r\n");
    printf("********************************************************************************\r\n");

    /* Initialize the SAR-ADC channels */
    InitAdcMultiChannel();

    /* Initialize the PWM counter channel */
    InitPwmAll();

    /* Trigger ADC conversion using TCPWM */
    StartConversion();

    for (;;)
    {
        /* Sample the input voltage for SARADC 0, 1, and 2 */
        if (g_flagSarGroupEoS == true)
        {
            /* Clear EoS interrupt flag */
            g_flagSarGroupEoS = false;

            /* Print the conversion value */
            printf("\x1b[1F");
            printf("\n\r ADC results:\tSAR0/CH0-%4d\tSAR1/CH0-%4d\tSAR2/CH0-%4d",
                   g_adcResultBuffer[0], g_adcResultBuffer[1], g_adcResultBuffer[2]);
        }
    }
}

/******************************************************************************
 * Function Name: HandleAdcGrpInt
 ******************************************************************************
 * Summary:
 *  This is the interrupt handler function for the ADC group-done conversion
 *  event. It fires when any one of ADC0, ADC1, or ADC2 finishes converting.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void HandleAdcGrpInt(void)
{
    /* Get interrupt status */
    uint32_t intrSource1 = Cy_SAR2_Channel_GetInterruptStatusMasked(ADC0_HW, ADC0_CH_0_IDX);
    uint32_t intrSource2 = Cy_SAR2_Channel_GetInterruptStatusMasked(ADC1_HW, ADC1_CH_0_IDX);
    uint32_t intrSource3 = Cy_SAR2_Channel_GetInterruptStatusMasked(ADC2_HW, ADC2_CH_0_IDX);

    /* If the interrupt is group-done */
    if (CY_SAR2_INT_GRP_DONE == (intrSource1 & CY_SAR2_INT_GRP_DONE))
    {
        /* Get ADC0 conversion result */
        g_adcResultBuffer[0] = GetAdcResult(ADC0_HW, ADC0_CH_0_IDX);

        /* Clear interrupt source */
        Cy_SAR2_Channel_ClearInterrupt(ADC0_HW, ADC0_CH_0_IDX, CY_SAR2_INT_GRP_DONE);
    }
    else if (CY_SAR2_INT_GRP_DONE == (intrSource2 & CY_SAR2_INT_GRP_DONE))
    {
        /* Get ADC1 conversion result */
        g_adcResultBuffer[1] = GetAdcResult(ADC1_HW, ADC1_CH_0_IDX);

        /* Clear interrupt source */
        Cy_SAR2_Channel_ClearInterrupt(ADC1_HW, ADC1_CH_0_IDX, CY_SAR2_INT_GRP_DONE);
    }
    else if (CY_SAR2_INT_GRP_DONE == (intrSource3 & CY_SAR2_INT_GRP_DONE))
    {
        /* Get ADC2 conversion result */
        g_adcResultBuffer[2] = GetAdcResult(ADC2_HW, ADC2_CH_0_IDX);

        /* Clear interrupt source */
        Cy_SAR2_Channel_ClearInterrupt(ADC2_HW, ADC2_CH_0_IDX, CY_SAR2_INT_GRP_DONE);
    }
    else
    {
        printf("Unexpected interrupt source");
        CY_ASSERT(0);
    }

    /* Set the TCPWM trigger to swap compare0 with the compare0 buffer */
    UpdateDutyRatio();
}

/******************************************************************************
 * Function Name: EnableTerminalOutput
 ******************************************************************************
 * Summary:
 *  Initializes the debug UART and sets up retarget-io so that printf() can
 *  be used to send terminal output over the debug UART.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void EnableTerminalOutput(void)
{
    /* API return code */
    cy_rslt_t rslt;

    /* Debug UART init */
    rslt = (cy_rslt_t)Cy_SCB_UART_Init(UART_HW, &UART_config, &UART_context);

    /* UART init failed. Stop program execution */
    HandleError(rslt);

    Cy_SCB_UART_Enable(UART_HW);

    /* Set up the HAL UART */
    rslt = mtb_hal_uart_setup(&UART_hal_obj, &UART_hal_config, &UART_context, NULL);

    /* HAL UART init failed. Stop program execution */
    HandleError(rslt);

    rslt = cy_retarget_io_init(&UART_hal_obj);

    /* HAL retarget_io init failed. Stop program execution */
    HandleError(rslt);
}

/******************************************************************************
 * Function Name: GetAdcResult
 ******************************************************************************
 * Summary:
 *  Gets the ADC conversion result for the specified channel and checks
 *  whether the conversion status is valid.
 *
 * Parameters:
 *  base - pointer to the SAR2 instance
 *  ch   - channel number of the SAR2 instance
 *
 * Return:
 *  ADC conversion result
 *
 *****************************************************************************/
static uint32_t GetAdcResult(PASS_SAR_Type *base, uint32_t ch)
{
    uint32_t adcStatus;

    /* Get the ADC conversion result and status */
    uint32_t adcResult = Cy_SAR2_Channel_GetResult(base, ch, &adcStatus);

    /* Check the status and raise conversion finish flag */
    if ((CY_SAR2_STATUS_VALID == (adcStatus & CY_SAR2_STATUS_VALID)))
    {
        g_flagSarGroupEoS = true;
    }
    else
    {
        printf("ADC conversion result is invalid\r\n");
        CY_ASSERT(0);
    }

    return adcResult;
}

/******************************************************************************
 * Function Name: UpdateDutyRatio
 ******************************************************************************
 * Summary:
 *  TCPWM trigger setting function.
 *  This function triggers the swap of the compare0 value with the compare0
 *  buffer value for PWM_U, 1, and 2.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void UpdateDutyRatio(void)
{
    {
        /* In an actual application, every PWM compare0 buffer value derived
         * by the FOC algorithm should be updated here, using functions such as:
         * Cy_TCPWM_PWM_SetCompare0BufVal(PWM_U_HW ,PWM_U_NUM, PWM_U_FEEDBACK_VALUE);
         * Cy_TCPWM_PWM_SetCompare0BufVal(PWM_V_HW ,PWM_V_NUM, PWM_V_FEEDBACK_VALUE);
         * Cy_TCPWM_PWM_SetCompare0BufVal(PWM_W_HW ,PWM_W_NUM, PWM_W_FEEDBACK_VALUE);
         */
    }

    /* Set a trigger to change the compare0 value to the compare0 buffer value
     * when the current PWM period ends
     */
    Cy_TCPWM_TriggerCaptureOrSwap_Single(PWM_U_HW, PWM_U_NUM);
    Cy_TCPWM_TriggerCaptureOrSwap_Single(PWM_V_HW, PWM_V_NUM);
    Cy_TCPWM_TriggerCaptureOrSwap_Single(PWM_W_HW, PWM_W_NUM);
}

/******************************************************************************
 * Function Name: InitAdcMultiChannel
 ******************************************************************************
 * Summary:
 *  ADC initialization function.
 *  This function initializes and configures the three channels belonging to
 *  SARADC 0, 1, and 2, respectively.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void InitAdcMultiChannel(void)
{
    /* De-initialize the SAR2 module */
    Cy_SAR2_DeInit(ADC0_HW);
    Cy_SAR2_DeInit(ADC1_HW);
    Cy_SAR2_DeInit(ADC2_HW);

    /* Initialize ADC */
    Cy_SAR2_Init(ADC0_HW, &ADC0_config);
    Cy_SAR2_Init(ADC1_HW, &ADC1_config);
    Cy_SAR2_Init(ADC2_HW, &ADC2_config);

    /* Set ADC group done interrupt */
    Cy_SAR2_Channel_SetInterruptMask(ADC0_HW, ADC0_CH_0_IDX, CY_SAR2_INT_GRP_DONE);
    Cy_SAR2_Channel_SetInterruptMask(ADC1_HW, ADC1_CH_0_IDX, CY_SAR2_INT_GRP_DONE);
    Cy_SAR2_Channel_SetInterruptMask(ADC2_HW, ADC2_CH_0_IDX, CY_SAR2_INT_GRP_DONE);

    /* Register ADC interrupt handler */
    Cy_SysInt_Init(&IRQ_CFG_ADC0, &HandleAdcGrpInt);
    Cy_SysInt_Init(&IRQ_CFG_ADC1, &HandleAdcGrpInt);
    Cy_SysInt_Init(&IRQ_CFG_ADC2, &HandleAdcGrpInt);

    /* Enable interrupt */
    NVIC_ClearPendingIRQ(ADC_IRQ_NUM);
    NVIC_EnableIRQ((IRQn_Type)ADC_IRQ_NUM);
}

/******************************************************************************
 * Function Name: InitPwmAll
 ******************************************************************************
 * Summary:
 *  TCPWM channel initialization function.
 *  This function initializes and configures TCPWM counter to trigger the
 *  SAR-ADC channels. In addition, three TCPWM PWM channels used to indicate
 *  the completion of the A/D conversion are initialized as well.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void InitPwmAll(void)
{
    /* Initialize all the TCPWM PWM channels used to indicate that each SAR-ADC
     * conversion has done
     */
    Cy_TCPWM_PWM_Init(PWM_U_HW, PWM_U_NUM, &PWM_U_config);
    Cy_TCPWM_PWM_Init(PWM_V_HW, PWM_V_NUM, &PWM_V_config);
    Cy_TCPWM_PWM_Init(PWM_W_HW, PWM_W_NUM, &PWM_W_config);
    Cy_TCPWM_PWM_Init(PWM_TRG_ADC_HW, PWM_TRG_ADC_NUM, &PWM_TRG_ADC_config);

    /* Enable all the TCPWM PWM */
    Cy_TCPWM_PWM_Enable(PWM_U_HW, PWM_U_NUM);
    Cy_TCPWM_PWM_Enable(PWM_V_HW, PWM_V_NUM);
    Cy_TCPWM_PWM_Enable(PWM_W_HW, PWM_W_NUM);
    Cy_TCPWM_PWM_Enable(PWM_TRG_ADC_HW, PWM_TRG_ADC_NUM);

    printf("TCPWM counter and PWM are configured...\r\n");
}

/******************************************************************************
 * Function Name: StartConversion
 ******************************************************************************
 * Summary:
 *  This function is used to trigger the counter to start the ADC conversion
 *  on three channels simultaneously.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  void
 *
 *****************************************************************************/
static void StartConversion(void)
{
    /* Start the TCPWM block simultaneously */
    cy_en_trigmux_status_t resultStatus;
    resultStatus = Cy_TrigMux_SwTrigger(TCPWM_ALL_TRIG_INPUT, CY_TRIGGER_TWO_CYCLES);
    HandleError(resultStatus);
    printf("PWM trigger started successfully...\r\n");
}

/* [] END OF FILE */
