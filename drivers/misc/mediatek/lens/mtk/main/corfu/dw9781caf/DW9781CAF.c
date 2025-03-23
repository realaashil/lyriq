/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/*
 * DW9781CAF voice coil motor driver
 *
 *
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>

#include <linux/slab.h>
#include "lens_info.h"
#include "dw9781_i2c.h"
#include "dw9781.h"

#define AF_DRVNAME "DW9781CAF_DRV"
#define AF_I2C_SLAVE_ADDR 0xE4

#define AF_DEBUG
#ifdef AF_DEBUG
#define LOG_INF(format, args...)                                               \
	pr_info(AF_DRVNAME " [%s] " format, __func__, ##args)
#else
#define LOG_INF(format, args...)
#endif

#define OIS_SWITCH 1

typedef enum {
    MCU_CMD_NONE           = 0x00,
    MCU_CMD_OIS_DISABLE    = 0x01,
} motOISParam;

static struct i2c_client *g_pstAF_I2Cclient;
static int *g_pAF_Opened;
static spinlock_t *g_pAF_SpinLock;

static unsigned long g_u4AF_INF;
static unsigned long g_u4AF_MACRO = 2046;
static unsigned long g_u4CurrPosition;
static motOISExtData *pDW9781TestResult = NULL;
extern int dw9781_check;

static int dw9781c_i2c_write(u8 *pwrite_data,u16 write_length)
{
	int i4RetValue = 0;
	struct i2c_msg msgs;

	msgs.addr  = AF_I2C_SLAVE_ADDR >> 1;
	msgs.flags = 0;
	msgs.len   = write_length;
	msgs.buf   = pwrite_data;

	i4RetValue = i2c_transfer(g_pstAF_I2Cclient->adapter, &msgs, 1);
	if (i4RetValue != 1) {
		LOG_INF("I2C send failed!!\n");
		return -1;
	}
	return 0;
}

static int s4AF_WriteReg(u16 addr, u16 data)
{
	int i4RetValue = 0;
	char pusendcmd[4] = {
		(char)(addr >> 8), (char)(addr & 0xFF), (char)(data >> 8),
		(char)(data & 0xFF)
	};

	i4RetValue = dw9781c_i2c_write(pusendcmd,4);
	return i4RetValue;
}

static inline int getAFInfo(__user struct stAF_MotorInfo *pstMotorInfo)
{
	struct stAF_MotorInfo stMotorInfo;

	stMotorInfo.u4MacroPosition = g_u4AF_MACRO;
	stMotorInfo.u4InfPosition = g_u4AF_INF;
	stMotorInfo.u4CurrentPosition = g_u4CurrPosition;
	stMotorInfo.bIsSupportSR = 1;

	stMotorInfo.bIsMotorMoving = 1;

	if (*g_pAF_Opened >= 1)
		stMotorInfo.bIsMotorOpen = 1;
	else
		stMotorInfo.bIsMotorOpen = 0;

	if (copy_to_user(pstMotorInfo, &stMotorInfo,
			 sizeof(struct stAF_MotorInfo)))
		LOG_INF("copy to user failed when getting motor information\n");

	return 0;
}

/* initAF include driver initialization and standby mode */
static int initAF(void)
{
	LOG_INF("+\n");

	if (*g_pAF_Opened == 1) {
		int i4RetValue = 0;

		if(!dw9781_check)
		{
			LOG_INF("OIS initialize failed");
			return -1;
		}

		ois_reset();

		i4RetValue = ois_checksum();
		if(i4RetValue < 0) {
			LOG_INF("dw9781 checksum failed!!\n");
			return -1;
		}

		ois_reset();

		LOG_INF("DW9781C OIS driver init success!!\n");

		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 2;
		spin_unlock(g_pAF_SpinLock);
	}

	LOG_INF("-\n");

	return 0;
}

/* moveAF only use to control moving the motor */
static inline int moveAF(unsigned long a_u4Position)
{
	int ret = 0;

	if (s4AF_WriteReg(0xD013,((u16)a_u4Position * 2)) == 0) {
		g_u4CurrPosition = (a_u4Position * 2);
		ret = 0;
	} else {
		LOG_INF("set I2C failed when moving the motor\n");
		ret = -1;
	}

	return ret;
}

static inline int setAFInf(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_INF = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

static inline int setAFMacro(unsigned long a_u4Position)
{
	spin_lock(g_pAF_SpinLock);
	g_u4AF_MACRO = a_u4Position;
	spin_unlock(g_pAF_SpinLock);
	return 0;
}

void control_ois(struct stAF_CtrlCmd *CtrlCmd)
{
	switch (CtrlCmd->i8CmdID) {
		case OIS_SWITCH:
			if(CtrlCmd->i8Param[0])
			{
				s4AF_WriteReg(0x7015,0x0001);
				LOG_INF("OIS OFF!\n");
			}
			else
			{
				s4AF_WriteReg(0x7015,0x0000);
				LOG_INF("OIS ON!\n");
			}
			break;

		default:
			break;
	}
}

/* ////////////////////////////////////////////////////////////// */
long DW9781CAF_Ioctl(struct file *a_pstFile, unsigned int a_u4Command,
		    unsigned long a_u4Param)
{
	long i4RetValue = 0;
	struct stAF_CtrlCmd ControlCmd;
	struct stAF_MotorCmd af_para;

	switch (a_u4Command) {
	case AFIOC_G_MOTORINFO:
		i4RetValue =
			getAFInfo((__user struct stAF_MotorInfo *)(a_u4Param));
		break;

	case AFIOC_T_MOVETO:
		i4RetValue = moveAF(a_u4Param);
		break;

	case AFIOC_T_SETINFPOS:
		i4RetValue = setAFInf(a_u4Param);
		break;

	case AFIOC_T_SETMACROPOS:
		i4RetValue = setAFMacro(a_u4Param);
		break;

	case AFIOC_X_CTRLPARA:
		copy_from_user(&ControlCmd, (__user struct stAF_CtrlCmd *)a_u4Param, sizeof(struct stAF_CtrlCmd));
		control_ois(&ControlCmd);
		break;

	case AFIOC_S_SETPARA:
		if (copy_from_user(&af_para, (__user struct stAF_MotorCmd *)a_u4Param, sizeof(struct stAF_MotorCmd))) {
			LOG_INF("OIS copy from user failed\n");
		}

		LOG_INF("OIS u4CmdID: %d, u4Param:%d\n", af_para.u4CmdID, af_para.u4Param);

		switch (af_para.u4CmdID) {
			case (u32)MCU_CMD_OIS_DISABLE:
				ControlCmd.i8CmdID = OIS_SWITCH;
				ControlCmd.i8Param[0] = af_para.u4Param;
				control_ois(&ControlCmd);
				break;
			default:
				break;
		}
		break;

	default:
		i4RetValue = -EPERM;
		break;
	}

	return i4RetValue;
}

/* Main jobs: */
/* 1.Deallocate anything that "open" allocated in private_data. */
/* 2.Shut down the device on last close. */
/* 3.Only called once on last time. */
/* Q1 : Try release multiple times. */
int DW9781CAF_Release(struct inode *a_pstInode, struct file *a_pstFile)
{
	LOG_INF("Start\n");

	if (*g_pAF_Opened == 2)
		LOG_INF("Wait\n");

	if (*g_pAF_Opened) {
		LOG_INF("Free\n");

		spin_lock(g_pAF_SpinLock);
		*g_pAF_Opened = 0;
		spin_unlock(g_pAF_SpinLock);
	}

	if (pDW9781TestResult) {
		kfree(pDW9781TestResult);
		pDW9781TestResult = NULL;
	}

	LOG_INF("End\n");

	return 0;
}

int DW9781CAF_SetI2Cclient(struct i2c_client *pstAF_I2Cclient,
			  spinlock_t *pAF_SpinLock, int *pAF_Opened)
{
	g_pstAF_I2Cclient = pstAF_I2Cclient;
	g_pAF_SpinLock = pAF_SpinLock;
	g_pAF_Opened = pAF_Opened;

	initAF();

	return 1;
}

int DW9781CAF_GetFileName(unsigned char *pFileName)
{
	#if SUPPORT_GETTING_LENS_FOLDER_NAME
	char FilePath[256];
	char *FileString;

	sprintf(FilePath, "%s", __FILE__);
	FileString = strrchr(FilePath, '/');
	*FileString = '\0';
	FileString = (strrchr(FilePath, '/') + 1);
	strncpy(pFileName, FileString, AF_MOTOR_NAME);
	LOG_INF("FileName : %s\n", pFileName);
	#else
	pFileName[0] = '\0';
	#endif
	return 1;
}

int MOT_DW9781CAF_EXT_CMD_HANDLER(motOISExtIntf *pExtCmd)
{
	dw9781_set_i2c_client(g_pstAF_I2Cclient);

	switch (pExtCmd->cmd) {
		case OIS_SART_FW_DL:
			LOG_INF("Kernel OIS_SART_FW_DL\n");
			break;
		case OIS_START_HEA_TEST:
			{
				if (!pDW9781TestResult) {
					pDW9781TestResult = (motOISExtData *)kzalloc(sizeof(motOISExtData), GFP_NOWAIT);
				}

				LOG_INF("Kernel OIS_START_HEA_TEST\n");
				if (pDW9781TestResult) {
					LOG_INF("OIS raius:%d,accuracy:%d,step/deg:%d,wait0:%d,wait1:%d,wait2:%d, ref_stroke:%d",
					        pExtCmd->data.hea_param.radius,
					        pExtCmd->data.hea_param.accuracy,
					        pExtCmd->data.hea_param.steps_in_degree,
					        pExtCmd->data.hea_param.wait0,
					        pExtCmd->data.hea_param.wait1,
					        pExtCmd->data.hea_param.wait2,
					        pExtCmd->data.hea_param.ref_stroke);
					square_motion_test(pExtCmd->data.hea_param.radius,
					                   pExtCmd->data.hea_param.accuracy,
					                   pExtCmd->data.hea_param.steps_in_degree,
					                   pExtCmd->data.hea_param.wait0,
					                   pExtCmd->data.hea_param.wait1,
					                   pExtCmd->data.hea_param.wait2,
					                   pExtCmd->data.hea_param.ref_stroke,
					                   pDW9781TestResult);
					LOG_INF("OIS HALL NG points:%d", pDW9781TestResult->hea_result.ng_points);
				} else {
					LOG_INF("FATAL: Kernel OIS_START_HEA_TEST memory error!!!\n");
				}
				break;
			}
		case OIS_START_GYRO_OFFSET_CALI:
			LOG_INF("Kernel OIS_START_GYRO_OFFSET_CALI\n");
			gyro_offset_calibrtion();
			break;
		default:
			LOG_INF("Kernel OIS invalid cmd\n");
			break;
	}
	return 0;
}

int MOT_DW9781CAF_GET_TEST_RESULT(motOISExtIntf *pExtCmd)
{
	switch (pExtCmd->cmd) {
		case OIS_QUERY_FW_INFO:
			LOG_INF("Kernel OIS_QUERY_FW_INFO\n");
			memset(&pExtCmd->data.fw_info, 0xff, sizeof(motOISFwInfo));
			break;
		case OIS_QUERY_HEA_RESULT:
			{
				LOG_INF("Kernel OIS_QUERY_HEA_RESULT\n");
				if (pDW9781TestResult) {
					memcpy(&pExtCmd->data.hea_result, &pDW9781TestResult->hea_result, sizeof(motOISHeaResult));
					LOG_INF("OIS NG points:%d, Ret:%d", pDW9781TestResult->hea_result.ng_points, pExtCmd->data.hea_result.ng_points);
				}
				break;
			}
		case OIS_QUERY_GYRO_OFFSET_RESULT:
			{
				motOISGOffsetResult *gOffset = dw9781_get_gyro_offset_result();
				LOG_INF("Kernel OIS_QUERY_GYRO_OFFSET_RESULT\n");
				memcpy(&pExtCmd->data.gyro_offset_result, gOffset, sizeof(motOISGOffsetResult));
				break;
			}
		default:
			LOG_INF("Kernel OIS invalid cmd\n");
			break;
	}
	return 0;
}

int MOT_DW9781CAF_SET_CALIBRATION(motOISExtIntf *pExtCmd)
{
	switch (pExtCmd->cmd) {
		case OIS_SET_GYRO_OFFSET:
			{
				if (pExtCmd->data.gyro_offset_result.is_success == 0 &&
				    pExtCmd->data.gyro_offset_result.x_offset != 0 &&
				    pExtCmd->data.gyro_offset_result.y_offset != 0) {
					motOISGOffsetResult *gOffset = dw9781_get_gyro_offset_result();

					//Update the gyro offset
					gOffset->is_success = 0;
					gOffset->x_offset = pExtCmd->data.gyro_offset_result.x_offset;
					gOffset->y_offset = pExtCmd->data.gyro_offset_result.y_offset;

					//Check if gyro offset update needed
					gyro_offset_check_update();
					LOG_INF("[%s] OIS update gyro_offset: %d,%d\n", __func__, gOffset->x_offset, gOffset->y_offset);
				}
			}
			break;
		default:
			break;
	}
	return 0;
}
