/*
 ## Cypress USB 3.0 Platform source file (cyfxuvcinmem.c)
 ## ===========================
 ##
 ##  Copyright Cypress Semiconductor Corporation, 2010-2018,
 ##  All Rights Reserved
 ##  UNPUBLISHED, LICENSED SOFTWARE.
 ##
 ##  CONFIDENTIAL AND PROPRIETARY INFORMATION
 ##  WHICH IS THE PROPERTY OF CYPRESS.
 ##
 ##  Use of this file is governed
 ##  by the license agreement included in the file
 ##
 ##     <install>/license/license.txt
 ##
 ##  where <install> is the Cypress software
 ##  installation root directory path.
 ##
 ## ===========================
*/

/* This file illustrates USB video class application example (streaming from internal memory) */

/*
   This example implements a USB video class Driver with the help of the appropriate USB enumeration
   descriptors. With these descriptors, the FX3 device enumerates as a USB Video Class device on the
   USB host.

   On successful enumeration the device shows up in the Windows Explorer. When the device is opened
   the host initiates a set of UVC specific class requests. The main class requests that need to be
   handled by the device are the GET/SET probe control request and SET commit control request. These
   request deal with the ISO bandwidth stream negotiation between the host and the device. In this
   example these requests are spoofed such that the host has only one alternate setting as the option.
   A predefined probe setting is returned as part of the Get Probe request. The Set probe / commit
   request is not interpreted and is only acknowledged.

   With successful stream negotiation the host issues request to switch to alternate setting 1 which
   starts the video streaming.

   The video streaming is accomplished with the help of a DMA MANUAL_OUT channel. Video frames are
   stored in contiguous memory location as a constant array. These frames are then loaded onto the
   DMA buffer one by one with appropriate UVC headers. With completion of each video frame the next
   indexed video frame is chosen for transfer. When all the frames are transferred, the index is reset
   to start transfer from the first video frame.

   CY_FX_UVC_STREAM_BUF_SIZE and CY_FX_UVC_STREAM_BUF_COUNT in the header file define the DMA buffer
   size and the number of DMA buffers respectively.

   This example is not supported on full speed interface.

   The example also implements a work-around for the FX3 device behavior of using the data PID
   for Hi-Speed ISO transfers based on the endpoint configuration alone. The device does not
   ensure that the correct PID is used when sending a short or zero length packet at the beginning
   of a micro-frame.

   The work-around implementation uses the data size retrieved from the FX3's EEPM_ENDPOINT register
   to dynamically update the ISO MULT value.
 */

#include "cyu3system.h"
#include "cyu3os.h"
#include "cyu3dma.h"
#include "cyu3error.h"
#include "cyfxuvcinmem.h"
#include "cyu3usb.h"
#include "cyu3uart.h"
#include "cyu3utils.h"

/* Setup data field : Request */
#define CY_U3P_USB_REQUEST_MASK                       (0x0000FF00)
#define CY_U3P_USB_REQUEST_POS                        (8)

/* Definitions for the DEV_EPI_CS register on FX3. */
#define FX3_USB2_INEP_CFG_ADDR_BASE     (0xe0031418)
#define FX3_USB2_INEP_MULT_MASK         (0x00003000)
#define FX3_USB2_INEP_MULT_POS          (12)

/* Definitions for the EEPM_ENDPOINT register on FX3. */
#define FX3_USB2_INEP_EPM_ADDR_BASE     (0xe0031c40)
#define FX3_USB2_INEP_EPM_READY_MASK    (0x40000000)
#define FX3_USB2_INEP_EPM_DSIZE_MASK    (0x07FFF800)
#define FX3_USB2_INEP_EPM_DSIZE_POS     (11)

CyU3PEpConfig_t uvcVideoEpCfg;
CyU3PThread uvcAppThread;                                       /* Thread structure */
static volatile uint8_t CurrentMultVal = 1;                     /* MULT value programmed into the EPM. */

/* UVC Header */
uint8_t glUVCHeader[CY_FX_UVC_MAX_HEADER] =
{
    0x0C,                           /* Header Length */
    0x8C,                           /* Bit field header field */
    0x00,0x00,0x00,0x00,            /* Presentation time stamp field */
    0x00,0x00,0x00,0x00,0x00,0x00   /* Source clock reference field */
};

/* Video Probe Commit Control */
uint8_t glCommitCtrl[CY_FX_UVC_MAX_PROBE_SETTING_ALIGNED] __attribute__ ((aligned (32)));

CyU3PDmaChannel          glChHandleUVCStream;           /* DMA Channel Handle  */
static volatile CyBool_t glIsApplnActive = CyFalse;     /* Whether the UVC application is active or not. */
static volatile CyBool_t glIsDevConfigured = CyFalse;   /* Whether the device has been configured. */

/* Application error handler */
void
CyFxAppErrorHandler (
        CyU3PReturnStatus_t apiRetStatus    /* API return status */
        )
{
    /* Application failed with the error code apiRetStatus */

    /* Add custom debug or recovery actions here */

    /* Loop indefinitely */
    for (;;)
    {
        /* Thread sleep : 100 ms */
        CyU3PThreadSleep (100);
    }
}

/* Set the MULT value for an ISO endpoint based on the EPM state. */
void
CyFxUvcAppSetMultByEpm (
        uint8_t ep)
{
    uint32_t val1 = *((uvint32_t *)(FX3_USB2_INEP_CFG_ADDR_BASE + (4 * ep)));
    uint32_t val2 = *((uvint32_t *)(FX3_USB2_INEP_EPM_ADDR_BASE + (4 * ep)));
    uint8_t  multVal = 0;

    /* If the EPM is ready, find out how much data is present and then update the MULT setting. */
    if ((val2 & FX3_USB2_INEP_EPM_READY_MASK) != 0)
    {
        val2 = (val2 & FX3_USB2_INEP_EPM_DSIZE_MASK) >> FX3_USB2_INEP_EPM_DSIZE_POS;
        multVal = (val2 / 1024) + 1;
    }

    CurrentMultVal = multVal;

    /* Adjust multVal to a value between 1 and 3. */
    multVal = CY_U3P_MIN (multVal, 3);
    multVal = CY_U3P_MAX (multVal, 1);

    val1 = (val1 & ~FX3_USB2_INEP_MULT_MASK) | (multVal << FX3_USB2_INEP_MULT_POS);
    *((uvint32_t *)(FX3_USB2_INEP_CFG_ADDR_BASE + (4 * ep))) = val1;
}

/* This function initializes the debug module for the UVC application */
void
CyFxUVCApplnDebugInit (void)
{
    CyU3PUartConfig_t uartConfig;
    CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

    /* Initialize the UART for printing debug messages */
    apiRetStatus = CyU3PUartInit();
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Set UART Configuration */
    uartConfig.baudRate = CY_U3P_UART_BAUDRATE_115200;
    uartConfig.stopBit = CY_U3P_UART_ONE_STOP_BIT;
    uartConfig.parity = CY_U3P_UART_NO_PARITY;
    uartConfig.txEnable = CyTrue;
    uartConfig.rxEnable = CyFalse;
    uartConfig.flowCtrl = CyFalse;
    uartConfig.isDma = CyTrue;

    /* Set the UART configuration */
    apiRetStatus = CyU3PUartSetConfig (&uartConfig, NULL);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Set the UART transfer */
    apiRetStatus = CyU3PUartTxSetBlockXfer (0xFFFFFFFF);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Initialize the debug application */
    apiRetStatus = CyU3PDebugInit (CY_U3P_LPP_SOCKET_UART_CONS, 8);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Disable the debug print header. */
    CyU3PDebugPreamble (CyFalse);
}

/* This callback is used to track whether the channel has committed any data to the endpoint. */
void CyFxUVCAppDmaCallback (
        CyU3PDmaChannel   *handle,
        CyU3PDmaCbType_t   type,
        CyU3PDmaCBInput_t *input)
{
    if (type == CY_U3P_DMA_CB_CONS_EVENT)
    {
        if (CyU3PUsbGetSpeed () == CY_U3P_HIGH_SPEED)
        {
            /* Update the ISO MULT setting based on the number of data packets ready in the EPM. */
            CyFxUvcAppSetMultByEpm (CY_FX_EP_ISO_VIDEO & 0x0F);
        }
    }
}

/* This function starts the video streaming application. It is called
 * when there is a SET_INTERFACE event for alternate interface 1. */
CyU3PReturnStatus_t
CyFxUVCApplnStart (void)
{
    CyU3PDmaChannelConfig_t dmaCfg;
    CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

    if (CyU3PUsbGetSpeed () == CY_U3P_SUPER_SPEED)
    {
        uvcVideoEpCfg.isoPkts  = CY_FX_EP_ISO_VIDEO_SS_MULT;
        uvcVideoEpCfg.burstLen = CY_FX_EP_ISO_VIDEO_SS_BURST;
    }
    else
    {
        /* Set the ISOMULT to 1 by default. This will be updated to the correct value when we start streaming data. */
        uvcVideoEpCfg.isoPkts  = 1;
        uvcVideoEpCfg.burstLen = 1;
    }

    /* Video streaming endpoint configuration */
    uvcVideoEpCfg.enable    = CyTrue;
    uvcVideoEpCfg.epType    = CY_U3P_USB_EP_ISO;
    uvcVideoEpCfg.pcktSize  = CY_FX_EP_ISO_VIDEO_PKT_SIZE;
    uvcVideoEpCfg.streams   = 0;

    apiRetStatus = CyU3PSetEpConfig(CY_FX_EP_ISO_VIDEO, &uvcVideoEpCfg);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "CyU3PSetEpConfig failed, Error Code = 0x%x\r\n", apiRetStatus);
        return apiRetStatus;
    }

    /* Create a DMA Manual OUT channel for streaming data */
    /* Video streaming Channel is not active till a stream request is received */
    dmaCfg.size = CY_FX_UVC_STREAM_BUF_SIZE;
    dmaCfg.count = CY_FX_UVC_STREAM_BUF_COUNT;
    dmaCfg.prodSckId = CY_U3P_CPU_SOCKET_PROD;
    dmaCfg.consSckId = CY_FX_EP_VIDEO_CONS_SOCKET;
    dmaCfg.dmaMode = CY_U3P_DMA_MODE_BYTE;
    dmaCfg.notification = CY_U3P_DMA_CB_CONS_EVENT;
    dmaCfg.cb = CyFxUVCAppDmaCallback;
    dmaCfg.prodHeader = 0;
    dmaCfg.prodFooter = 0;
    dmaCfg.consHeader = 0;
    dmaCfg.prodAvailCount = 0;
    apiRetStatus = CyU3PDmaChannelCreate (&glChHandleUVCStream, CY_U3P_DMA_TYPE_MANUAL_OUT, &dmaCfg);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "CyU3PDmaChannelCreate failed, error code = %d\r\n",apiRetStatus);
        return apiRetStatus;
    }

    /* Flush the endpoint memory */
    CyU3PUsbFlushEp(CY_FX_EP_ISO_VIDEO);

    apiRetStatus = CyU3PDmaChannelSetXfer (&glChHandleUVCStream, 0);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "CyU3PDmaChannelSetXfer failed, error code = %d\r\n", apiRetStatus);
        return apiRetStatus;
    }

    /* Update the flag so that the application thread is notified of this. */
    glIsApplnActive = CyTrue;
    CyU3PDebugPrint(3, "App Started\r\n");
    return CY_U3P_SUCCESS;
}

/* This function stops the video streaming. It is called from the USB event
 * handler, when there is a reset / disconnect or SET_INTERFACE for alternate
 * interface 0. */
void
CyFxUVCApplnStop (void)
{
    /* Update the flag so that the application thread is notified of this. */
    glIsApplnActive = CyFalse;

    /* Abort and destroy the video streaming channel */
    CyU3PDmaChannelDestroy (&glChHandleUVCStream);

    /* Flush the endpoint memory */
    CyU3PUsbFlushEp(CY_FX_EP_ISO_VIDEO);

    /* Disable the video streaming endpoint. */
    uvcVideoEpCfg.enable = CyFalse;
    CyU3PSetEpConfig(CY_FX_EP_ISO_VIDEO, &uvcVideoEpCfg);

    CyU3PDebugPrint(3, "App Stopped\r\n");
}

/* This is the Callback function to handle the USB Events */
static void
CyFxUVCApplnUSBEventCB (
    CyU3PUsbEventType_t evtype, /* Event type */
    uint16_t            evdata  /* Event data */
    )
{
    uint8_t interface = 0, altSetting = 0;

    switch (evtype)
    {
        case CY_U3P_USB_EVENT_SETCONF:
            if (glIsApplnActive)
                CyFxUVCApplnStop ();
            if (evdata != 0)
                glIsDevConfigured = CyTrue;
            break;

        case CY_U3P_USB_EVENT_SETINTF:
            /* Start the video streamer application if the interface requested was 1. If not, stop the streamer. */
            interface = CY_U3P_GET_MSB(evdata);
            altSetting = CY_U3P_GET_LSB(evdata);

            /* Stop the application before re-starting. */
            if (glIsApplnActive)
            {
                CyFxUVCApplnStop ();
            }

            /* Start the video stream if the streaming interface has been selected. */
            if ((interface == CY_FX_UVC_INTERFACE_VS) && (altSetting != 0))
            {
                CyFxUVCApplnStart ();
            }
            break;

        case CY_U3P_USB_EVENT_RESET:
        case CY_U3P_USB_EVENT_DISCONNECT:
            /* Stop the video streamer application. */
            if (glIsApplnActive)
            {
                CyFxUVCApplnStop ();
            }
            glIsDevConfigured = CyFalse;
            break;

        default:
            break;
    }
}

/* Callback to handle the USB Setup Requests and UVC Class events */
static CyBool_t
CyFxUVCApplnUSBSetupCB (
        uint32_t setupdat0, /* SETUP Data 0 */
        uint32_t setupdat1  /* SETUP Data 1 */
    )
{
    uint16_t readCount = 0;
    uint8_t  bRequest, bReqType;
    uint8_t  bType, bTarget;
    uint16_t wValue, wIndex;
    CyBool_t isHandled = CyFalse;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    uint8_t  temp = 0;

    /* Fast enumeration is used. Only requests addressed to the interface, class,
     * vendor and unknown control requests are received by this function. */

    /* Decode the fields from the setup request. */
    bReqType = (setupdat0 & CY_U3P_USB_REQUEST_TYPE_MASK);
    bType    = (bReqType & CY_U3P_USB_TYPE_MASK);
    bTarget  = (bReqType & CY_U3P_USB_TARGET_MASK);
    bRequest = ((setupdat0 & CY_U3P_USB_REQUEST_MASK) >> CY_U3P_USB_REQUEST_POS);
    wValue   = ((setupdat0 & CY_U3P_USB_VALUE_MASK)   >> CY_U3P_USB_VALUE_POS);
    wIndex   = ((setupdat1 & CY_U3P_USB_INDEX_MASK)   >> CY_U3P_USB_INDEX_POS);

    if (bType == CY_U3P_USB_STANDARD_RQT)
    {
        /* Handle SET_FEATURE(FUNCTION_SUSPEND) and CLEAR_FEATURE(FUNCTION_SUSPEND)
         * requests here. It should be allowed to pass if the device is in configured
         * state and failed otherwise. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && ((bRequest == CY_U3P_USB_SC_SET_FEATURE)
                    || (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)) && (wValue == 0))
        {
            if (glIsDevConfigured)
                CyU3PUsbAckSetup ();
            else
                CyU3PUsbStall (0, CyTrue, CyFalse);

            isHandled = CyTrue;
        }
    }

    /* Check for UVC Class Requests */
    if (bType == CY_U3P_USB_CLASS_RQT)
    {
        CyU3PDebugPrint (4, "UVC RQT: %x %x %x %x %x\r\n", bTarget, bRequest, CY_U3P_GET_MSB(wIndex),
                CY_U3P_GET_LSB(wIndex), wValue);

        /* Handle requests addressed to the Video Control interface. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && (CY_U3P_GET_LSB (wIndex) == CY_FX_UVC_INTERFACE_VC))
        {
            /* Respond to VC_REQUEST_ERROR_CODE_CONTROL and stall every other request as this example does not support
               any of the Video Control features */
            if ((CY_U3P_GET_MSB(wIndex) == 0x00) && (wValue == CY_FX_USB_UVC_VC_RQT_ERROR_CODE_CONTROL))
            {
                temp      = CY_FX_USB_UVC_RQT_STAT_INVALID_CTRL;
                isHandled = CyTrue;
                CyU3PUsbSendEP0Data (0x01, &temp);
            }
        }

        /* Handle requests addressed to the Video Streaming interface. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && (CY_U3P_GET_LSB (wIndex) == CY_FX_UVC_INTERFACE_VS))
        {
            isHandled = CyTrue;

            switch (wValue)
            {
                /* As we have a single setting, we treat both PROBE and COMMIT control requests in the same way.
                 * The commit controls sent down by the host are ignored, as we can only stream with a single
                 * setting.
                 */
                case CY_FX_USB_UVC_VS_PROBE_CONTROL:
                case CY_FX_USB_UVC_VS_COMMIT_CONTROL:
                    {
                        switch (bRequest)
                        {
                            /* We only have one functional setting. Keep returning the same as current, default
                             * minimum and maximum. */
                            case CY_FX_USB_UVC_GET_CUR_REQ:
                            case CY_FX_USB_UVC_GET_DEF_REQ:
                            case CY_FX_USB_UVC_GET_MIN_REQ:
                            case CY_FX_USB_UVC_GET_MAX_REQ:
                                status = CyU3PUsbSendEP0Data (CY_FX_UVC_MAX_PROBE_SETTING,
                                        (uint8_t *)glProbeCtrl);
                                if (status != CY_U3P_SUCCESS)
                                {
                                    CyU3PDebugPrint (4, "CyU3PUsbSendEP0Data, error code = %d\n", status);
                                }
                                break;

                            case CY_FX_USB_UVC_SET_CUR_REQ:
                                /* Disable the low power entry to optimize USB throughput */
                                CyU3PUsbLPMDisable();

                                /* Read the data out into a local buffer. We do not use this data in any way. */
                                status = CyU3PUsbGetEP0Data (CY_FX_UVC_MAX_PROBE_SETTING_ALIGNED,
                                        glCommitCtrl, &readCount);
                                if (status != CY_U3P_SUCCESS)
                                {
                                    CyU3PDebugPrint (4, "CyU3PUsbGetEP0Data failed, error code = %d\n", status);
                                }
                                else
                                {
                                    /* Check the read count. Expecting a count of CY_FX_UVC_MAX_PROBE_SETTING bytes. */
                                    if (readCount != (uint16_t)CY_FX_UVC_MAX_PROBE_SETTING)
                                    {
                                        CyU3PDebugPrint (4, "Invalid number of bytes received in SET_CUR Request");
                                    }
                                }
                                break;

                            default:
                                CyU3PUsbStall (0, CyTrue, CyFalse);
                                break;
                        }
                    }
                    break;

                default:
                    CyU3PUsbStall (0, CyTrue, CyFalse);
                    break;
            }
        }

        /* Don't try to stall the endpoint if we have already attempted data transfer. */
    }

    return isHandled;
}

/* Callback function to handle LPM requests from the USB 3.0 host. This function is invoked by the API
   whenever a state change from U0 -> U1 or U0 -> U2 happens. If we return CyTrue from this function, the
   FX3 device is retained in the low power state. If we return CyFalse, the FX3 device immediately tries
   to trigger an exit back to U0.

   This application does not have any state in which we should not allow U1/U2 transitions; and therefore
   the function always return CyTrue.
 */
CyBool_t
CyFxApplnLPMRqtCB (
        CyU3PUsbLinkPowerMode link_mode)
{
    return CyTrue;
}

/* This function initializes the USB Module, creates event group,
   sets the enumeration descriptors, configures the Endpoints and
   configures the DMA module for the UVC Application */
void
CyFxUVCApplnInit (void)
{
    CyU3PEpConfig_t endPointConfig;
    CyU3PReturnStatus_t apiRetStatus = CY_U3P_SUCCESS;

    /* Start the USB functionality */
    apiRetStatus = CyU3PUsbStart();
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Function Failed to Start, Error Code = %d\r\n",apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* The fast enumeration is the easiest way to setup a USB connection,
     * where all enumeration phase is handled by the library. Only the
     * class / vendor requests need to be handled by the application. */
    CyU3PUsbRegisterSetupCallback(CyFxUVCApplnUSBSetupCB, CyTrue);

    /* Setup the callback to handle the USB events */
    CyU3PUsbRegisterEventCallback(CyFxUVCApplnUSBEventCB);

    /* Register a callback to handle LPM requests from the USB 3.0 host. */
    CyU3PUsbRegisterLPMRequestCallback(CyFxApplnLPMRqtCB);    
    
    /* Set the USB Enumeration descriptors */

    /* High speed device descriptor. */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_DEVICE_DESCR, 0, (uint8_t *)CyFxUSB20DeviceDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set device descriptor failed, Error code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* BOS descriptor */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_SS_BOS_DESCR, 0, (uint8_t *)CyFxUSBBOSDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set configuration descriptor failed, Error code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* High speed configuration descriptor */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_HS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBHSConfigDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Set Other Speed Descriptor failed, Error Code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Full speed configuration descriptor */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_FS_CONFIG_DESCR, 0, (uint8_t *)CyFxUSBFSConfigDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB Set Configuration Descriptor failed, Error Code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* String descriptor 0 */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 0, (uint8_t *)CyFxUSBStringLangIDDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* String descriptor 1 */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 1, (uint8_t *)CyFxUSBManufactureDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* String descriptor 2 */
    apiRetStatus = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 2, (uint8_t *)CyFxUSBProductDscr);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB set string descriptor failed, Error code = %d\r\n", apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Since the status interrupt endpoint is not used in this application,
     * just enable the EP in the beginning. */
    /* Control status interrupt endpoint configuration */
    endPointConfig.enable = 1;
    endPointConfig.epType = CY_U3P_USB_EP_INTR;
    endPointConfig.pcktSize = 64;
    endPointConfig.isoPkts  = 1;
    endPointConfig.burstLen = 1;

    apiRetStatus = CyU3PSetEpConfig(CY_FX_EP_CONTROL_STATUS, &endPointConfig);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "CyU3PSetEpConfig failed, error code = %d\r\n",apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }

    /* Connect the USB pins and enable super speed operation */
    apiRetStatus = CyU3PConnectState(CyTrue, CyFalse);
    if (apiRetStatus != CY_U3P_SUCCESS)
    {
        CyU3PDebugPrint (4, "USB connect failed, Error Code = %d\r\n",apiRetStatus);
        CyFxAppErrorHandler(apiRetStatus);
    }
}

/* UVC header addition function */
static void
CyFxUVCAddHeader (
        uint8_t *buffer_p, /* Buffer pointer */
        uint8_t frameInd   /* EOF or normal frame indication */
    )
{
    /* Copy header to buffer */
    CyU3PMemCopy (buffer_p, (uint8_t *)glUVCHeader, CY_FX_UVC_MAX_HEADER);

    /* Check if last packet of the frame. */
    if (frameInd == CY_FX_UVC_HEADER_EOF)
    {
        /* Modify UVC header to toggle Frame ID */
        glUVCHeader[1] ^= CY_FX_UVC_HEADER_FRAME_ID;

        /* Indicate End of Frame in the buffer */
        buffer_p[1] |=  CY_FX_UVC_HEADER_EOF;
    }
}

/* Entry function for the UVC application thread. */
void
UVCAppThread_Entry (
        uint32_t input)
{
    CyU3PDmaBuffer_t dmaBuffer;
    uint16_t commitLength = 0;
    uint32_t frameStart = 0, frameIndex = 0, frameOffset = 0;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Initialize the Debug Module */
    CyFxUVCApplnDebugInit();

    /* Initialize the UVC Application */
    CyFxUVCApplnInit();

    for (;;)
    {
        frameStart = 0;
        frameIndex = 0;
        frameOffset = 0;

        /* Reset Frame Id in UVC Header */
        glUVCHeader[1] = CY_FX_UVC_HEADER_DEFAULT_BFH;

        /* Video streamer application. */
        while (glIsApplnActive)
        {
            /* Wait for a free buffer. */
            status = CyU3PDmaChannelGetBuffer (&glChHandleUVCStream,
                    &dmaBuffer, CYU3P_WAIT_FOREVER);
            if (status != CY_U3P_SUCCESS)
            {
                break;
            }

            /* Check if packet is last packet or first/intermediate packet */
            if (frameOffset + (CY_FX_UVC_STREAM_BUF_SIZE - CY_FX_UVC_MAX_HEADER) <
                    glVidFrameLen[frameIndex])
            {
                /* Load the video data to the OUT buffer */
                CyU3PMemCopy ((dmaBuffer.buffer + CY_FX_UVC_MAX_HEADER),
                        (uint8_t *)&glUVCVidFrames[frameStart + frameOffset],
                        (CY_FX_UVC_STREAM_BUF_SIZE - CY_FX_UVC_MAX_HEADER));

                /* Add header with normal frame indication */
                CyFxUVCAddHeader (dmaBuffer.buffer, CY_FX_UVC_HEADER_FRAME);

                /* Commit buffer length */
                CyU3PThreadSleep (3);
                commitLength = CY_FX_UVC_STREAM_BUF_SIZE;

                if (CyU3PUsbGetSpeed () == CY_U3P_HIGH_SPEED)
                {
                    /* If the current mult setting does not match the expected data size, update this in a safe
                       manner. */
                    if (CurrentMultVal != CY_FX_EP_ISO_VIDEO_PKTS_COUNT)
                    {
                        CyU3PUsbSetEpNak (CY_FX_EP_ISO_VIDEO, CyTrue);
                        CyU3PBusyWait (10);
                        status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                        CyU3PBusyWait (20);
                        CyFxUvcAppSetMultByEpm (CY_FX_EP_ISO_VIDEO & 0x0F);
                        CyU3PUsbSetEpNak (CY_FX_EP_ISO_VIDEO, CyFalse);
                    }
                    else
                    {
                        /* No change to mult setting. Just commit the data. */
                        status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                    }
                }
                else
                {
                    /* Not Hi-speed operation. Just commit the data. */
                    status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                }

                if (status != CY_U3P_SUCCESS)
                {
                    break;
                }

                /* Update the index for video data */
                frameOffset += (CY_FX_UVC_STREAM_BUF_SIZE - CY_FX_UVC_MAX_HEADER);
            }
            else
            {
                /* Last packet of the video frame. Send this data and then reset all counters. */

                /* Load the video data to the OUT buffer */
                CyU3PMemCopy (dmaBuffer.buffer + CY_FX_UVC_MAX_HEADER,
                        (uint8_t *)&glUVCVidFrames[frameStart + frameOffset],
                        (glVidFrameLen[frameIndex] - frameOffset));

                /* Commit buffer length */
                CyU3PThreadSleep (3);
                commitLength = (glVidFrameLen[frameIndex] - frameOffset)
                    + CY_FX_UVC_MAX_HEADER;

                /* Add the header with End of Frame Indication */
                CyFxUVCAddHeader (dmaBuffer.buffer, CY_FX_UVC_HEADER_EOF);

                if (CyU3PUsbGetSpeed () == CY_U3P_HIGH_SPEED)
                {
                    /* If the current mult setting does not match the expected data size, update this in a safe
                       manner. */
                    if (CurrentMultVal != ((commitLength / 1024) + 1))
                    {
                        CyU3PUsbSetEpNak (CY_FX_EP_ISO_VIDEO, CyTrue);
                        CyU3PBusyWait (10);
                        status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                        CyU3PBusyWait (20);
                        CyFxUvcAppSetMultByEpm (CY_FX_EP_ISO_VIDEO & 0x0F);
                        CyU3PUsbSetEpNak (CY_FX_EP_ISO_VIDEO, CyFalse);
                    }
                    else
                    {
                        /* No change to mult setting. Just commit the data. */
                        status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                    }
                }
                else
                {
                    /* Not Hi-speed operation. Just commit the data. */
                    status = CyU3PDmaChannelCommitBuffer (&glChHandleUVCStream, commitLength, 0);
                }

                if (status != CY_U3P_SUCCESS)
                {
                    break;
                }

                /* Reset the Index for the next frame */
                frameOffset = 0;
                frameStart += glVidFrameLen[frameIndex];
                frameIndex++;

                /* If all frames are transferred then start from 0 */
                if (frameIndex >= CY_FX_UVC_MAX_VID_FRAMES)
                {
                    frameIndex = 0;
                    frameStart = 0;
                }
            }
        }

        /* There is a streamer error. Flag it. */
        if ((status != CY_U3P_SUCCESS) && (glIsApplnActive))
        {
            CyU3PDebugPrint (4, "UVC video streamer error. Code %d.\r\n", status);
            CyFxAppErrorHandler (status);
        }

        /* Sleep for sometime as video streamer is idle. */
        CyU3PThreadSleep (100);

    } /* End of for(;;) */
}

/* Application define function which creates the threads. */
void
CyFxApplicationDefine (
        void)
{
    void *ptr = NULL;
    uint32_t retThrdCreate = CY_U3P_SUCCESS;

    /* Allocate the memory for the thread and create the thread */
    ptr = CyU3PMemAlloc (UVC_APP_THREAD_STACK);
    retThrdCreate = CyU3PThreadCreate (&uvcAppThread,   /* UVC Thread structure */
                           "30:UVC_app_thread",         /* Thread Id and name */
                           UVCAppThread_Entry,          /* UVC Application Thread Entry function */
                           0,                           /* No input parameter to thread */
                           ptr,                         /* Pointer to the allocated thread stack */
                           UVC_APP_THREAD_STACK,        /* UVC Application Thread stack size */
                           UVC_APP_THREAD_PRIORITY,     /* UVC Application Thread priority */
                           UVC_APP_THREAD_PRIORITY,     /* Pre-emption threshold */
                           CYU3P_NO_TIME_SLICE,         /* No time slice for the application thread */
                           CYU3P_AUTO_START             /* Start the Thread immediately */
                           );

    /* Check the return code */
    if (retThrdCreate != 0)
    {
        /* Thread Creation failed with the error code retThrdCreate */

        /* Add custom recovery or debug actions here */

        /* Application cannot continue */
        /* Loop indefinitely */
        while(1);
    }
}

/*
 * Main function
 */
int
main (void)
{
    CyU3PIoMatrixConfig_t io_cfg;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Initialize the device */
    status = CyU3PDeviceInit (NULL);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* Initialize the caches. Enable only the Instruction Cache. */
    status = CyU3PDeviceCacheControl (CyTrue, CyFalse, CyFalse);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* Configure the IO matrix for the device. On the FX3 DVK board, the COM port 
     * is connected to the IO(53:56). This means that either DQ32 mode should be
     * selected or lppMode should be set to UART_ONLY. Here we are choosing
     * UART_ONLY configuration. */
    CyU3PMemSet ((uint8_t *)&io_cfg, 0, sizeof (io_cfg));
    io_cfg.isDQ32Bit = CyFalse;
    io_cfg.s0Mode    = CY_U3P_SPORT_INACTIVE;
    io_cfg.s1Mode    = CY_U3P_SPORT_INACTIVE;
    io_cfg.useUart   = CyTrue;
    io_cfg.useI2C    = CyFalse;
    io_cfg.useI2S    = CyFalse;
    io_cfg.useSpi    = CyFalse;
    io_cfg.lppMode   = CY_U3P_IO_MATRIX_LPP_UART_ONLY;

    /* No GPIOs are enabled. */
    io_cfg.gpioSimpleEn[0]  = 0;
    io_cfg.gpioSimpleEn[1]  = 0;
    io_cfg.gpioComplexEn[0] = 0;
    io_cfg.gpioComplexEn[1] = 0;
    status = CyU3PDeviceConfigureIOMatrix (&io_cfg);
    if (status != CY_U3P_SUCCESS)
    {
        goto handle_fatal_error;
    }

    /* This is a non returnable call for initializing the RTOS kernel */
    CyU3PKernelEntry ();

    /* Dummy return to make the compiler happy */
    return 0;

handle_fatal_error:

    /* Cannot recover from this error. */
    while (1);
}

/* [ ] */

