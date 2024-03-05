

#include <huawei_platform/log/log_jank.h>
#include "../../huawei_ts_kit_algo.h"
#include "focaltech_flash.h"
#include "focaltech_core.h"

#if defined(CONFIG_HUAWEI_DSM)
#include <dsm/dsm_pub.h>
#endif

char module_ini_name[64] = {0};
EXPORT_SYMBOL(module_ini_name);

static int focal_enter_work_model_from_pram(struct focal_platform_data *focal_pdata);
static int focal_enter_pram_model(struct focal_platform_data *focal_pdata);
static int focal_flash_get_fw_file_size(char *fw_name);
static int focal_flash_read_fw_file(char *fw_name, u8 *fw_buf);
static int focal_read_vendor_id_in_pram(u8 *vendor_id);

static int focal_enter_work_model_form_pram_update(
	struct focal_platform_data *focal_pdata);
static int focal_flash_pram(struct focal_platform_data *focal_pdata,
	const u8 *pram_data, size_t pram_size);
static int focal_firmware_update(struct focal_platform_data *focal_pdata,
	const u8 *pbt_buf, u32 dw_lenth);
static int focal_get_data_check_sum(const u8 *data, size_t data_size,
	u8 *check_sum);
static int focal_enter_work_model_from_rom_update(
	struct focal_platform_data *focal_pdata);
static int focal_read_check_sum_in_pram(struct focal_platform_data *focal_pdata,
	u32 start_addr, u32 crc_length,
	u8 *check_sum);

static int focal_enter_work_model_by_hardware(void);

/*
 * description : get ic status
 *
 * param - status : buffer to receive ic status
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_get_status(u32 *status)
{
	int ret = 0;

	u8 cmd = 0x00;
	u8 reg_val[2] = {0};

	if (NULL == status) {
		TS_LOG_ERR("%s:input parameter is null\n", __func__);
		return -ENOMEM;
	}

	memset(reg_val, 0, sizeof(reg_val));
	cmd = FTS_CMD_GET_STATUS;

	ret = focal_read(&cmd, 1, reg_val, 2);
	if (ret) {
		TS_LOG_ERR("%s:read status fail, ret=%d\n", __func__, ret);
		return ret;
	}

	*status = (reg_val[0] << 8) | (reg_val[1]);

	return 0;
}

/*
 * description : enter bootloader model by hardware reset ic
 *
 * param - focal_data : struct focal_platform_data *focal_pdata
 *
 * return : return 0 if read ic status success, otherwize return error code
 *
 * notice : this function only work in normal model
 */
static int focal_enter_rom_update_model_by_hardware(
	struct focal_platform_data *focal_pdata)
{
	return focal_hardware_reset_to_rom_update_model();
}

static int focal_enter_pram_update_model_by_hardware(
	struct focal_platform_data *focal_pdata)
{
	return focal_hardware_reset_to_pram_update_model();
}

/*
 * return: if read project id success, return 0, else return error code
 *
 * notice: this function work when ic is in normal model
 */
int focal_enter_rom_update_model_by_software(struct focal_platform_data *focal_pdata)
{
	int ret = 0;

	ret = focal_write_reg(FTS_REG_SOFT_RESET_FC, FTS_UPGRADE_AA);
	if (ret < 0) {
		TS_LOG_ERR("%s:write 0xAA to soft reset ic fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	ret = focal_write_reg(FTS_REG_SOFT_RESET_FC, FTS_UPGRADE_55);
	if (ret < 0) {
		TS_LOG_ERR("%s:write 0x55 to soft reset ic fail, ret=%d\n",
			__func__, ret);
		return ret;
	}
	msleep(focal_pdata->delay_time->reboot_delay);

	return 0;
}


/*
 * description : check if firmware package have write to ic
 *
 * param - command : firmware write command, such as FTS_CMD_WRITE_PRAM
 *
 * param - start_addr : addr in ic to receive the first firmware package
 *
 * param - start_write_addr : addr in ic to receive current firmware pachage
 *
 * param - data_size : current package data size
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_wait_firmware_write_finish(
	struct focal_platform_data *focal_pdata,
	u8 command,
	u32 start_addr,
	u32 start_write_addr,
	u32 data_size)
{
	int i = 0;
	int ret = 0;
	u32 ic_status = 0;
	u32 driver_status = 0;

	/* no need to wait when write pram */
	if (FTS_CMD_WRITE_PRAM == command)
		return 0;

	driver_status = start_addr + start_write_addr / data_size;
	for (i = 0; i < focal_pdata->delay_time->write_flash_query_times; i++) {

		ret = focal_get_status(&ic_status);
		if (ret) {
			TS_LOG_ERR("%s:get ic status fail, ret=%d\n",
				__func__, ret);
			return ret;
		}

		if (ic_status == driver_status)
			return 0;

		msleep(1);
	}


	TS_LOG_ERR("%s:%s, ic status=0x%X, driver status=0x%X\n",
			__func__, "time out for check ic status",
			ic_status, driver_status);

	return -ETIMEDOUT;
}

/*
 * description : write firmware data to ic
 *
 * param - fw_data : firmware data to write
 *
 * param - fw_length : firmware data length
 *
 * param - command : firmware write command, like FTS_CMD_WRITE_PRAM
 *
 * param - start_addr : addr in ic that first firmware package to write
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_write_firmware_data(
	struct focal_platform_data *focal_pdata,
	const u8 *fw_data,
	u32 fw_length,
	u8 command,
	u32 start_addr)
{
	int i = 0;
	int ret = 0;

	u32 data_size = 0;
	u32 package_count = 0;
	u32 copy_offset = 0;
	u32 start_write_addr = 0;

	u8 package_buf[FTS_PACKAGE_SIZE + 6] = {0};

	TS_LOG_DEBUG("%s:Write firmware to ic\n", __func__);
	TS_LOG_DEBUG("%s:fw write:data_length=%u, package_count=%u\n",
		__func__, fw_length, package_count);

	package_count = fw_length / FTS_PACKAGE_SIZE;
	if (fw_length % FTS_PACKAGE_SIZE != 0)
		package_count += 1;

	start_write_addr = start_addr;
	package_buf[0] = command;
	for (i = 0; i < package_count; i++) {

		/* the last package to write */
		if ((i + 1 == package_count)
			&& (fw_length % FTS_PACKAGE_SIZE != 0)) {
			data_size = fw_length % FTS_PACKAGE_SIZE;
		} else {
			data_size = FTS_PACKAGE_SIZE;
		}

		/* offset */
		package_buf[1] = (u8)(start_write_addr >> 16);
		package_buf[2] = (u8)(start_write_addr >> 8);
		package_buf[3] = (u8)(start_write_addr);

		/* data size */
		package_buf[4] = (u8)(data_size >> 8);
		package_buf[5] = (u8)(data_size);

		copy_offset = start_write_addr - start_addr;
		memcpy(&package_buf[6],	&fw_data[copy_offset], data_size);

		ret = focal_write(package_buf, data_size + 6);
		if (ret < 0) {
			TS_LOG_ERR("%s:write fw data fail, index=%d, ret=%d\n",
				__func__, i, ret);
			return -EIO;
		}

		ret = focal_wait_firmware_write_finish(focal_pdata,
			command, start_addr, start_write_addr, data_size);
		if (ret) {
			TS_LOG_ERR("%s:%s, ret=%d\n", __func__,
				"wait firmware write finish fail", ret);
			return ret;
		}

		start_write_addr += data_size;
		TS_LOG_DEBUG("%s:%s:index=%d, data_size=%u, writed_size=%u\n",
			__func__, "fw write", i, data_size, start_write_addr);
	}

	return 0;
}

/*
 * description : write pram data to ic
 *
 * param - fw_data : firmware data to write
 *
 * param - fw_length : firmware data length
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_write_pram_data(
	struct focal_platform_data *focal_pdata,
	const u8 *fw_data,
	u32 fw_length)
{
	return focal_write_firmware_data(focal_pdata, fw_data, fw_length,
		FTS_CMD_WRITE_PRAM, 0);
}

/*
 * description : write app data to ic
 *
 * param - fw_data : firmware data to write
 *
 * param - fw_length : firmware data length
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_write_app_data(
	struct focal_platform_data *focal_pdata,
	const u8 *fw_data,
	u32 fw_length)
{
	return focal_write_firmware_data(focal_pdata, fw_data, fw_length,
		FTS_CMD_WRITE_FLASH, FTS_FW_IC_ADDR_START);
}

/*
 * description : read pram in ic to buf
 *
 * param - buf : buffer to receive pram data
 *
 * param - size : size
 *
 * param - pram_size : firmware data length
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_read_pram_from_ic(
	u8 *buf,
	u32 size,
	u32 pram_size)
{
	int i = 0;
	int ret = 0;

	u32 data_size = 0;
	u32 package_count = 0;
	u32 readed_size = 0;

	if (size < pram_size) {
		TS_LOG_ERR("%s:buffer is not enough to receive pram\n",
			__func__);
		TS_LOG_ERR("%s:buffer size=%u, pram size=%u\n", __func__,
			size, pram_size);
		return -ENOMEM;
	}

	package_count = pram_size / FTS_PACKAGE_SIZE;
	if (pram_size % FTS_PACKAGE_SIZE != 0)
		package_count += 1;

	TS_LOG_DEBUG("%s:pram_size=%u, package_count=%u\n", __func__,
		pram_size, package_count);

	for (i = 0; i < package_count; i++) {

		if ((i + 1 == package_count)
			&& (pram_size % FTS_PACKAGE_SIZE != 0)) {
			data_size = pram_size % FTS_PACKAGE_SIZE;
		} else {
			data_size = FTS_PACKAGE_SIZE;
		}

		ret = focal_read_pram_package(readed_size, &buf[readed_size],
			data_size);
		if (ret) {
			TS_LOG_ERR("%s:read pram package error\n", __func__);
			TS_LOG_ERR("%s:read_size=%u, data_size=%u, index=%d\n",
				__func__, readed_size, data_size, i);
			return ret;
		}

		readed_size += data_size;
	}

	return 0;
}

/*
 * description : read pram in ic to buf
 *
 * param - buf : buffer to receive pram data
 *
 * param - size : size
 *
 * param - pram_size : firmware data length
 *
 * return : return 0 if read ic status success, otherwize return error code
 */
static int focal_erasure_app_area(struct focal_platform_data *focal_pdata)
{
	int i = 0;
	int ret = 0;
	u32 ic_state = 0;
	u8 cmd = 0x00;

	cmd = FTS_CMD_ERASURE_APP;
	ret = focal_write(&cmd, 1);
	if (ret) {
		TS_LOG_ERR("%s:send erase command fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	msleep(focal_pdata->delay_time->erase_min_delay);

	for (i = 0; i < focal_pdata->delay_time->erase_flash_query_times; i++) {

		ret = focal_get_status(&ic_state);
		if (ret) {
			TS_LOG_ERR("%s:get ic status fail, ret=%d\n",
				__func__, ret);
		} else {
			if (FTS_ERASURE_OK_STATUS == ic_state)
				return 0;
		}

		msleep(focal_pdata->delay_time->erase_query_delay);
	}

	return -EIO;
}

/*
 * return : if success, return 0, otherwize return error code
 *
 * notice : this function is working when ic is in pram model
 */
static int focal_enter_pram_update_model_from_pram(void)
{
	int ret = 0;
	u8 cmd[2] = {0};

	cmd[0] = FTS_CMD_SET_MODE;
	cmd[1] = FTS_UPGRADE_MODE;

	ret = focal_write(cmd, 2);
	if (ret) {
		TS_LOG_ERR("%s:set pram update model fail, ret = %d\n",
			__func__, ret);
		return ret;
	}

	msleep(50);

	return 0;
}

/*
 * return : if success, return 0, otherwize return error code
 */
static int focal_auto_config_clock(void)
{
	int ret = 0;
	u8 reg_val = 0;
	u8 cmd[3] = {0};

	cmd[0] = FTS_REG_FLASH_TYPE;
	ret = focal_read(cmd, 1, &reg_val, 1);
	if (ret) {
		TS_LOG_ERR("%s:read flash type fail, ret=%d\n", __func__, ret);
		return ret;
	}

	cmd[0] = FTS_REG_FLASH_TYPE;
	cmd[1] = reg_val;
	cmd[2] = 0x00;

	ret = focal_write(cmd, 3);
	if (ret) {
		TS_LOG_ERR("%s:%s, flash type=0x%02x, ret=%d\n", __func__,
			"write flash type fail",
			reg_val, ret);
		return ret;
	}

	return 0;
}

/*
 * param - start_addr : addr in ic to start check sum
 *
 * param - crc_length : length of data to check sum
 *
 * param - check_sum : buffer to receive check sum value
 *
 * return : if success, return 0, otherwize return error code
 */
static int focal_read_check_sum(
	struct focal_platform_data *focal_pdata,
	u32 start_addr,
	u32 crc_length,
	u8 *check_sum)
{
	int i = 0;
	int ret = 0;

	u32 ic_status = 0;

	u8 cmd[6] = {0};
	u8 reg_val = 0;

	if (crc_length > LEN_FLASH_ECC_MAX) {
		TS_LOG_ERR("%s:%s, crc_length=%u, max=%d\n",
			__func__, "crc length out of range",
			crc_length, LEN_FLASH_ECC_MAX);
		return -EINVAL;
	}

	/* start verify */
	cmd[0] = FTS_CMD_CALC_CRC;
	ret = focal_write(cmd, 1);
	if (ret) {
		TS_LOG_ERR("%s:start verify fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	msleep(focal_pdata->delay_time->calc_crc_delay);

	cmd[0] = FTS_CMD_SET_CALC_ADDR;

	cmd[1] = (u8)(start_addr >> 16);
	cmd[2] = (u8)(start_addr >> 8);
	cmd[3] = (u8)(start_addr);

	cmd[4] = (u8)(crc_length >> 8);
	cmd[5] = (u8)(crc_length);

	ret = focal_write(cmd, 6);
	if (ret) {
		TS_LOG_ERR("%s:write verify parameter fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	msleep(crc_length / 256);

	cmd[0] = FTS_CMD_GET_STATUS;
	for (i = 0; i < focal_pdata->delay_time->read_ecc_query_times; i++) {

		ret = focal_get_status(&ic_status);
		if (ret) {
			TS_LOG_ERR("%s:get ic status fail, ret=%d\n",
				__func__, ret);
		} else {
			if (FTS_ECC_OK_STATUS == ic_status)
				break;
		}

		if (i == focal_pdata->delay_time->read_ecc_query_times - 1) {
			TS_LOG_ERR("%s:%s, out of max retry times\n",
				__func__, "status check fail");
		}

		msleep(1);
	}

	cmd[0] = FTS_CMD_READ_CRC;
	ret = focal_read(cmd, 1, &reg_val, 1);
	if (ret) {
		TS_LOG_ERR("%s:read crc fail, ret=%d\n", __func__, ret);
		return ret;
	}

	*check_sum = reg_val;

	return 0;
}

/*
 * param - flash_type : buffer to receive flash type
 *
 * return : if success, return 0, otherwize return error code
 */
static int focal_read_flash_type(u8 *flash_type)
{
	int ret = 0;

	u8 cmd = 0;

	cmd = FTS_REG_FLASH_TYPE;
	ret = focal_read(&cmd, 1, flash_type, 1);
	if (ret) {
		TS_LOG_ERR("%s:read flash type fail, ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}

/*
 * description : focal may use two kinds of memery,
 *               one is winbond and other one is ft5003,
 *               but almost not use ft5003, because ft5003 is made by focal,
 *               and the performance is not very will.
 *
 * param - fw_len : firmware size
 *
 * return : if success, return 0, otherwize return error code
 */
static int focal_check_firmware_size_in_pram_model(u32 fw_len)
{
	int ret = 0;
	u8 flash_type = 0;
	u32 max_fw_len = 0;

	ret = focal_read_flash_type(&flash_type);
	if (ret) {
		TS_LOG_ERR("%s:read flash type fail, ret=%d\n", __func__, ret);
		return ret;
	}

	TS_LOG_INFO("%s:flash type=0x%X\n", __func__, flash_type);

	switch (flash_type) {
	case FTS_FLASH_TYPE_FT5003:
		max_fw_len = FTS_FLASH_MAX_LEN_FT5003;
		break;
	case FTS_FLASH_TYPE_WINBOND:
		max_fw_len = FTS_FLASH_MAX_LEN_WINBOND;
		break;
	default:
		TS_LOG_ERR("%s:%s, flash_type=0x%X\n", __func__,
			"no flash type maech, use default", flash_type);
		max_fw_len = FTS_FLASH_MAX_LEN_WINBOND;
	}

	TS_LOG_INFO("%s:fw_len=%u, max_fw_len=%u\n", __func__,
		fw_len, max_fw_len);

	if (max_fw_len < fw_len) {
		TS_LOG_ERR("%s:firmware length check fail\n", __func__);
		return -ENODATA;
	}

	return 0;
}


#define CTP_IC_TYPE_FT5436	0X54
#define TP_OUFEI      			0x34

#define FT_UPGRADE_AA       	0xAA
#define FT_UPGRADE_55       	0x55
#define FT_REG_RESET_FW     0x07

#define FT_MAX_WR_BUF       10
#define FT_MAX_RD_BUF       2
#define FT_FW_PKT_LEN       		128
#define FT_FW_PKT_META_LEN  	6
#define AUTO_CLB_NONEED     	0

#define FT_EARSE_DLY_MS     100
#define FT_55_AA_DLY_NS     5000

#define FT_READ_ID_REG      0x90
#define FT_ERASE_APP_REG    0x61
#define FT_ERASE_PANEL_REG  0x63
#define FT_FW_START_REG     0xBF

#define FT_REG_ID       		0xA3
#define FT_UPGRADE_LOOP     30


int hid_to_i2c(void)
{
	u8 auc_i2c_write_buf[5] = {0};
	u8 auc_i2c_read_buf[5] = {0};
	int ret = 0;

	auc_i2c_write_buf[0] = 0xeb;
	auc_i2c_write_buf[1] = 0xaa;
	auc_i2c_write_buf[2] = 0x09;
	focal_write(auc_i2c_write_buf, 3);
	msleep(10);

	auc_i2c_write_buf[0] = auc_i2c_write_buf[1] = auc_i2c_write_buf[2] = 0;
	focal_read(auc_i2c_write_buf, 0, auc_i2c_read_buf, 3);
	
	TS_LOG_INFO("auc_i2c_read_buf[0]:%x,auc_i2c_read_buf[1]:%x,auc_i2c_read_buf[2]:%x\n",auc_i2c_read_buf[0],auc_i2c_read_buf[1],auc_i2c_read_buf[2]);
	if(0xeb==auc_i2c_read_buf[0] && 0xaa==auc_i2c_read_buf[1] && 0x08==auc_i2c_read_buf[2])
		ret = 1;		
	else 
		ret = 0;

	return ret;
}


struct fw_upgrade_info {
	bool auto_cal;
	u16 delay_aa;
	u16 delay_55;
	u8 upgrade_id_1;
	u8 upgrade_id_2;
	u16 delay_readid;
	u16 delay_erase_flash;
};

static int ft5x06_fw_upgrade_start(struct i2c_client *client,
                                   const u8 *data, u32 data_len)
{
	struct fw_upgrade_info info;
	u8 reg_addr;
	u8 chip_id = 0x00;
	u8 w_buf[FT_MAX_WR_BUF] = {0}, r_buf[FT_MAX_RD_BUF] = {0};
	u8 pkt_buf[FT_FW_PKT_LEN + FT_FW_PKT_META_LEN];
	int i, j, temp;
	u32 pkt_num, pkt_len;
	u8 fw_ecc;
	int i_ret;

	reg_addr = FT_REG_ID;
	temp = focal_read(&reg_addr, 1, &chip_id, 1);
	TS_LOG_INFO("Update:Read ic info:%x\n",chip_id);
	if (temp < 0)
	{
	    TS_LOG_ERR("version read failed");
	}

	info.auto_cal = AUTO_CLB_NONEED;
	info.delay_55 = 2;
	info.delay_aa = 2;
	info.upgrade_id_1 = 0x54;
	info.upgrade_id_2 = 0x2c;
	info.delay_readid = 10;
	info.delay_erase_flash = 1350;

	/* determine firmware size */
	i_ret = hid_to_i2c();
	if(i_ret == 0)
	{
		TS_LOG_INFO("[FTS] hid change to i2c fail ! \n");
	}

	for (i = 0, j = 0; i < FT_UPGRADE_LOOP; i++)
	{
		msleep(FT_EARSE_DLY_MS);

		focal_write_reg(0xfc, FT_UPGRADE_AA);
		msleep(info.delay_aa);

		//write 0x55 to register 0xfc 
		focal_write_reg(0xfc, FT_UPGRADE_55);
		msleep(200);

		i_ret = hid_to_i2c();
		if(i_ret == 0)
		{
			TS_LOG_INFO("[FTS] hid change to i2c fail ! \n");
		}

		msleep(10);
		/* Enter upgrade mode */
		w_buf[0] = FT_UPGRADE_55;
		focal_write(&w_buf[0], 1);
		usleep(FT_55_AA_DLY_NS);
		w_buf[0] = FT_UPGRADE_AA;
		focal_write(&w_buf[0], 1);
		if(i_ret < 0)
		{
			TS_LOG_INFO("[FTS] failed writing  0x55 and 0xaa ! \n");
			continue;
		}

		/* check READ_ID */
		msleep(info.delay_readid);
		w_buf[0] = FT_READ_ID_REG;
		w_buf[1] = 0x00;
		w_buf[2] = 0x00;
		w_buf[3] = 0x00;

		focal_read(w_buf, 4, r_buf, 2);

		TS_LOG_INFO("r_buf[0] :%X,r_buf[1]: %X\n", r_buf[0], r_buf[1]);
		if (r_buf[0] != info.upgrade_id_1
		    || r_buf[1] != info.upgrade_id_2)
		{
		    TS_LOG_INFO("Upgrade ID mismatch(%d), IC=0x%x 0x%x, info=0x%x 0x%x\n",
		            i, r_buf[0], r_buf[1],
		            info.upgrade_id_1, info.upgrade_id_2);
		}
		else
		    break;
	}
	
	TS_LOG_INFO("Begin to update \n\n");
	if (i >= FT_UPGRADE_LOOP)
	{
		TS_LOG_INFO("Abort upgrade\n");
		return -EIO;
	}

	/* erase app and panel paramenter area */
	TS_LOG_INFO("Step 4:erase app and panel paramenter area\n");
	w_buf[0] = FT_ERASE_APP_REG;
	focal_write(w_buf, 1);
	msleep(1350);

	for(i = 0;i < 15;i++)
	{
		w_buf[0] = 0x6a;
		r_buf[0] = r_buf[1] = 0x00;
		focal_read(w_buf, 1, r_buf, 2);
	    
		if(0xF0==r_buf[0] && 0xAA==r_buf[1])
		{
			break;
		}
		msleep(50);
	}
	 
	w_buf[0] = 0xB0;
	w_buf[1] = (u8) ((data_len >> 16) & 0xFF);
	w_buf[2] = (u8) ((data_len >> 8) & 0xFF);
	w_buf[3] = (u8) (data_len & 0xFF);
	focal_write( w_buf, 4);

	/* program firmware */
	TS_LOG_INFO("Step 5:program firmware\n");
	pkt_num = (data_len) / FT_FW_PKT_LEN;
	pkt_buf[0] = FT_FW_START_REG;
	pkt_buf[1] = 0x00;
	fw_ecc = 0;
	temp = 0;
    
    for (j = 0; j < pkt_num; j++)
    {
        temp = j * FT_FW_PKT_LEN;
        pkt_buf[2] = (u8) (temp >> 8);
        pkt_buf[3] = (u8) temp;
		pkt_len = FT_FW_PKT_LEN;
        pkt_buf[4] = (u8) (pkt_len >> 8);
        pkt_buf[5] = (u8) pkt_len;

        for (i = 0; i < FT_FW_PKT_LEN; i++)
        {
            pkt_buf[6 + i] = data[j * FT_FW_PKT_LEN + i];
            fw_ecc ^= pkt_buf[6 + i];
        }

	focal_write(pkt_buf,FT_FW_PKT_LEN + 6);

	for(i = 0;i < 30;i++)
	{
		w_buf[0] = 0x6a;
		r_buf[0] = r_buf[1] = 0x00;
		focal_read(w_buf, 1, r_buf, 2);

		if ((j + 0x1000) == (((r_buf[0]) << 8) | r_buf[1]))
		{
			break;
		}
		msleep(1);
	}
    }

    /* send remaining bytes */
	TS_LOG_INFO("Step 6:send remaining bytes\n");
    if ((data_len) % FT_FW_PKT_LEN > 0)
    {
        temp = pkt_num * FT_FW_PKT_LEN;
        pkt_buf[2] = (u8) (temp >> 8);
        pkt_buf[3] = (u8) temp;
        temp = (data_len) % FT_FW_PKT_LEN;
        pkt_buf[4] = (u8) (temp >> 8);
        pkt_buf[5] = (u8) temp;

        for (i = 0; i < temp; i++)
        {
            pkt_buf[6 + i] = data[pkt_num * FT_FW_PKT_LEN + i];
            fw_ecc ^= pkt_buf[6 + i];
        }

	focal_write(pkt_buf, temp + 6);

	for(i = 0;i < 30;i++)
	{
		w_buf[0] = 0x6a;
		r_buf[0] = r_buf[1] = 0x00;
		focal_read(w_buf, 1, r_buf, 2);

		if ((j + 0x1000) == (((r_buf[0]) << 8) | r_buf[1]))
		{
			break;
		}
		msleep(1);
	}
    }

	msleep(50);
						
	/*********Step 6: read out checksum***********************/
	/*send the opration head */
	TS_LOG_INFO("Step 7: read out checksum\n");
	w_buf[0] = 0x64;
	focal_write(w_buf, 1); 
	msleep(300);

	temp = 0;
	w_buf[0] = 0x65;
	w_buf[1] = (u8)(temp >> 16);
	w_buf[2] = (u8)(temp >> 8);
	w_buf[3] = (u8)(temp);
	temp = data_len;
	w_buf[4] = (u8)(temp >> 8);
	w_buf[5] = (u8)(temp);
	i_ret = focal_write(w_buf, 6); 
	msleep(data_len/256);

	for(i = 0;i < 100;i++)
	{
		w_buf[0] = 0x6a;
		r_buf[0] = r_buf[1] = 0x00;
		focal_read(w_buf, 1, r_buf, 2);

		if (0xF0==r_buf[0] && 0x55==r_buf[1])
		{
			break;
		}

		msleep(1);
	}

	w_buf[0] = 0x66;
	focal_read(w_buf, 1, r_buf, 1);
	if (r_buf[0] != fw_ecc) 
	{
		TS_LOG_ERR("[FTS]--ecc error! FW=%02x bt_ecc=%02x\n", r_buf[0], fw_ecc);
		return -EIO;
	}

	/* reset */
	TS_LOG_INFO("Step 8: reset \n");
	w_buf[0] = FT_REG_RESET_FW;
	focal_write(w_buf, 1);
	msleep(200);

	TS_LOG_INFO("Firmware upgrade successful\n");
	return 0;
}


#ifdef  CTP_IC_TYPE_FT5436
static int focal_firmware_update(
	struct focal_platform_data *focal_pdata,
	const u8 *fw_data,
	u32 fw_len)
{
	int ret = 0;
       u8 ic_type;
	u8 reg_addr;

	reg_addr = 0xA3;
	focal_read_reg(reg_addr, &ic_type);

	//need check
	if(ic_type != CTP_IC_TYPE_FT5436)
	{
		TS_LOG_ERR("IC type dismatch, please check");
	}
	
	if ((fw_data[fw_len - 8] ^ fw_data[fw_len - 6]) == 0xFF
	    && (fw_data[fw_len - 7] ^ fw_data[fw_len - 5]) == 0xFF
	    && (fw_data[fw_len - 3] ^ fw_data[fw_len - 4]) == 0xFF)
	{
		TS_LOG_ERR("[%s: upgrade_start \n",__func__);
		ret = ft5x06_fw_upgrade_start(g_focal_dev_data->ts_platform_data->client, fw_data, fw_len);

		if (ret != 0)
		{
		    TS_LOG_ERR("[FTS]  upgrade failed rc = %d.\n", ret);
		}
		else
		{
		    TS_LOG_ERR("[FTS] upgrade successfully.\n");
		}
	}

	return ret;
}

#else
/*
 * description : give this function the firmware data,
 *               and the lengthe of the firmware data,
 *               this function can finish firmeare update
 *
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - fw_data : firmware data to update
 *
 * param - fw_len : the length of firmware data
 *
 * return : if success, return 0, otherwize return error code
 */
static int focal_firmware_update(
	struct focal_platform_data *focal_pdata,
	const u8 *fw_data,
	u32 fw_len)
{
	int ret = 0;
	u8 check_sum_of_fw = 0;
	u8 check_sum_in_ic = 0;

	ret = focal_enter_pram_model(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:enter pram model fail, ret=%d\n", __func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_enter_pram_model_fail;
#endif
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:enter pram model success\n", __func__);
	}

	ret = focal_check_firmware_size_in_pram_model(fw_len);
	if (ret) {
		TS_LOG_ERR("%s:check firmware size fail, ret-%d\n",
			__func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_check_firmware_size_in_pram_model_fail;
#endif
		goto enter_work_model_from_pram;
	}

	focal_get_data_check_sum(fw_data, fw_len, &check_sum_of_fw);

	ret = focal_auto_config_clock();
	if (ret) {
		TS_LOG_ERR("%s:config clock fail, ret=%d\n", __func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_auto_config_clock_fail;
#endif
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:config clock success\n", __func__);
	}

	ret = focal_enter_pram_update_model_from_pram();
	if (ret) {
		TS_LOG_ERR("%s:enter update model fail, ret=%d\n",
			__func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_enter_pram_update_model_from_pram_fail;
#endif
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:enter update model success\n", __func__);
	}

	ret = focal_erasure_app_area(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:erasure app fail, ret=%d\n", __func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_erasure_app_area_fail;
#endif
		goto enter_work_model_from_update_model;
	} else {
		TS_LOG_INFO("%s:erasure app success\n", __func__);
	}

	ret = focal_write_app_data(focal_pdata, fw_data, fw_len);
	if (ret) {
		TS_LOG_ERR("%s:write app data fail, ret=%d\n", __func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_write_app_data_fail;
#endif		
		goto enter_work_model_from_update_model;
	} else {
		TS_LOG_INFO("%s:write app data success\n", __func__);
	}

	ret = focal_read_check_sum(focal_pdata, FTS_FW_IC_ADDR_START,
		fw_len, &check_sum_in_ic);
	if (ret) {
		TS_LOG_ERR("%s:read check sum in ic fail, ret=%d\n",
			__func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
		g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status =  focal_read_check_sum_fail;
#endif	
		goto enter_work_model_from_update_model;
	}

	TS_LOG_INFO("%s:crc_in_ic=0x%02X, crc_of_fw=0x%02X\n",
			__func__, check_sum_in_ic, check_sum_of_fw);

	if (check_sum_in_ic != check_sum_of_fw) {
		TS_LOG_ERR("%s:check sum check fail\n", __func__);
		ret = -EIO;
	}

enter_work_model_from_update_model:
	if (focal_enter_work_model_form_pram_update(focal_pdata)) {
		TS_LOG_ERR("%s:enter work model fail, ret=%d\n", __func__, ret);
	} else {
		TS_LOG_INFO("%s:enter work model success\n", __func__);
		return ret;
	}

enter_work_model_from_pram:
	if (focal_enter_work_model_from_pram(focal_pdata)) {
		TS_LOG_ERR("%s:enter work mode from pram fail, ret=%d\n",
			__func__, ret);
	} else {
		TS_LOG_INFO("%s:enter work mode from pram success\n", __func__);
	}

	return ret;
}
#endif

/*
 * param - fw_file_path : firmware file path
 *
 * return : if success, return file size, other wize return error code
 */
static int focal_flash_get_fw_file_size(char *fw_file_path)
{
	off_t file_size = 0;
	unsigned long magic = 0;

	struct file *pfile = NULL;
	struct inode *inode = NULL;

	if (NULL == fw_file_path) {
		TS_LOG_ERR("%s:fw file path is null\n", __func__);
		return -EINVAL;
	}

	pfile = filp_open(fw_file_path, O_RDONLY, 0);
	if (IS_ERR(pfile)) {
		TS_LOG_ERR("%s:fw file open fail, path=%s\n",
			__func__, fw_file_path);
		return -EIO;
	}

	inode = pfile->f_path.dentry->d_inode;
	magic = inode->i_sb->s_magic;
	file_size = inode->i_size;
	filp_close(pfile, NULL);

	return file_size;
}

/*
 * param - fw_file_path : fw file path
 *
 * param - fw_buf : buff to store firmware file data
 *
 * return : if success, return 0, otherwize return error code
 */
static int focal_flash_read_fw_file(
	char *fw_file_path,
	u8 *fw_buf)
{
	loff_t pos = 0;
	off_t fsize = 0;
	mm_segment_t old_fs;
	unsigned long magic = 0;

	struct file *pfile = NULL;
	struct inode *inode = NULL;

	if (NULL == fw_file_path || NULL == fw_buf) {
		TS_LOG_ERR("%s:input parameter is null\n", __func__);
		return -EINVAL;
	}

	pfile = filp_open(fw_file_path, O_RDONLY, 0);
	if (IS_ERR(pfile)) {
		TS_LOG_ERR("%s:open firmware file fail, path=%s\n",
			__func__, fw_file_path);
		return -EIO;
	}

	inode = pfile->f_path.dentry->d_inode;
	magic = inode->i_sb->s_magic;
	fsize = inode->i_size;
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	vfs_read(pfile, fw_buf, fsize, &pos);
	filp_close(pfile, NULL);
	set_fs(old_fs);

	return NO_ERR;
}

/*
 * param - project_id : the vendor id value
 *
 * param - pram_name : buff to store pram file name
 *
 * param - size : size of #pram_name
 *
 * return : if success, return file size, other wize return error code
 */
int focal_get_pram_file_name(
	struct focal_platform_data *focal_pdata,
	char *pram_name,
	size_t size)
{
	int ret = 0;

	if (NULL == focal_pdata || NULL == pram_name) {
		TS_LOG_ERR("%s:input parameter is null\n", __func__);
		return -EINVAL;
	}

	ret = focal_read_chip_id_from_rom(focal_pdata, &focal_pdata->chip_id);
	if (ret) {
		TS_LOG_ERR("%s:read chip id fail, ret=%d\n", __func__, ret);
		return ret;
	}
	return snprintf(pram_name, size, "ts/%s_focal_pram_%x.img",
		focal_pdata->focal_device_data->ts_platform_data->product_name,
		focal_pdata->chip_id);

}

/*
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_start_app_from_rom_update_model(void)
{
	int ret = 0;

	ret = focal_write_default(FTS_CMD_START_APP);
	if (ret < 0) {
		TS_LOG_INFO("%s:start app fail\n", __func__);
		return ret;
	}

	msleep(10);

	return 0;
}

/*
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_start_pram(void)
{
	int i = 0;
	int ret = 0;

	u32 chip_id = 0;
	u8 cmd[2] = {0};

	/* 1. start pram */
	TS_LOG_INFO("%s:start pram\n", __func__);
	ret = focal_start_app_from_rom_update_model();
	if (ret) {
		TS_LOG_INFO("%s:start pram fail, ret=%d\n", __func__, ret);
		return ret;
	}

	/* 2. check if pram have start success */
	for (i = 0; i < 5; i++) {

		cmd[0] = FTS_UPGRADE_55;
		cmd[1] = FTS_UPGRADE_AA;
		ret = focal_write(cmd, 2);
		if (ret < 0) {
			TS_LOG_INFO("%s:write command fail, ret=%d\n",
				__func__, ret);
			continue;
		}

		msleep(10);

		ret = focal_read_chip_id_(&chip_id);
		if (ret || chip_id == 0) {
			TS_LOG_ERR("%s:chip id read fail, retry=%d\n",
				__func__, i);
			continue;
		} else {
			TS_LOG_INFO("%s:check chip id success\n",
				__func__);
			return 0;
		}
	}

	return -EINVAL;

}

/*
 * param focal_pdata - focal_platform_data *focal_pdata
 *
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_enter_pram_model(
	struct focal_platform_data *focal_pdata)
{
	int ret = 0;

	/* enter bootloader */
	TS_LOG_DEBUG("%s:enter rom update model\n", __func__);
	ret = focal_enter_pram_update_model_by_hardware(focal_pdata);
	if (ret) {
		focal_hardware_reset_to_normal_model();
		return -ENODEV;
	}

	return 0;
}

/*
 * param - focal_pdata : focal_platform_data *focal_pdata
 *
 * param - pram_data : pram firmware
 *
 * param - pram_size : pram firmware size
 *
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_flash_pram(
	struct focal_platform_data *focal_pdata,
	const u8 *pram_data,
	size_t pram_size)
{
	int i = 0;
	int ret = 0;
	u8 *pcheck_buffer = NULL;

	TS_LOG_DEBUG("%s:pram file size is %lu\n", __func__, pram_size);
	if (pram_size > FTS_MAX_PRAM_SIZE || pram_size == 0) {
		TS_LOG_ERR("pram file size overflow %lu\n", pram_size);
		return -EINVAL;
	}

	TS_LOG_DEBUG("%s:write pram data\n", __func__);
	ret = focal_write_pram_data(focal_pdata, pram_data, pram_size);
	if (ret) {
		TS_LOG_ERR("%s: write pram data fail, ret=%d\n", __func__, ret);
		return ret;
	}

	msleep(100);

	pcheck_buffer = kmalloc(pram_size, GFP_ATOMIC);
	if (NULL == pcheck_buffer) {
		TS_LOG_ERR("%s: malloc mem for pcheck_buffer fail\n", __func__);
		return -ENOMEM;
	}

	TS_LOG_DEBUG("%s:read pram data from ic\n", __func__);
	ret = focal_read_pram_from_ic(pcheck_buffer, pram_size, pram_size);
	if (ret) {
		TS_LOG_ERR("%s: read pram data from ic fail, ret=%d\n",
			__func__, ret);
		goto release_pcheck_buffer;
	}

	for (i = 0; i < pram_size; i++) {
		if (pcheck_buffer[i] != pram_data[i]) {
			TS_LOG_ERR("%s:%s,index=%d, src=0x%02x, cut=0x%02x\n",
				__func__, "pram write check fail,",
				i, pram_data[i], pcheck_buffer[i]);
			ret = -EINVAL;
			goto release_pcheck_buffer;
		}
	}

	ret = 0;

release_pcheck_buffer:
	if (pcheck_buffer != NULL) {
		kfree(pcheck_buffer);
		pcheck_buffer = NULL;
	}

	return ret;
}

/*
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - fw_name : firmware file path
 *
 * return : if success, return 0, otherwize return error code
 */
int focal_flash_upgrade_with_bin_file(
	struct focal_platform_data *focal_pdata,
	char *fw_name)
{
	int ret = 0;
	int fw_size = 0;
	u8 fw_ver = 0;
	u8 *pbt_buf = NULL;

	fw_size = focal_flash_get_fw_file_size(fw_name);
	if (fw_size <= 0) {
		TS_LOG_ERR("%s:get fw file size fail, ret=%d, fw_name=%s\n",
			__func__, fw_size, fw_name);
		return fw_size;
	}

	pbt_buf = kzalloc(fw_size + 1, GFP_ATOMIC);
	if (NULL == pbt_buf) {
		TS_LOG_ERR("%s:alloc memory fail\n", __func__);
		return -ENOMEM;
	}

	if (focal_flash_read_fw_file(fw_name, pbt_buf)) {
		TS_LOG_ERR("%s:read fw file data fail\n", __func__);
		ret = -EIO;
		goto free_buf;
	}

	 /* call the write pram function */
	ret = focal_enter_pram_model(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:write pram fail i_ret = %d\n", __func__, ret);
		goto enter_work_model;
	} else {
		TS_LOG_INFO("%s:wirte pram success\n", __func__);
	}

	/* call upgrade function */
	ret = focal_firmware_update(focal_pdata, pbt_buf, fw_size);
	if (ret != 0) {
		TS_LOG_ERR("%s:upgrade failed. err=%d.\n", __func__, ret);
		goto enter_work_model;
	} else {
		TS_LOG_INFO("%s:upgrade successful.\n", __func__);
	}

	ret = focal_get_ic_firmware_version(&fw_ver);
	if (ret) {
		TS_LOG_ERR("%s:get firmware version fail, ret=%d\n",
			__func__, ret);
		goto enter_work_model;
	} else {
		focal_pdata->fw_ver = fw_ver;
	}

enter_work_model:
	focal_enter_work_model();

free_buf:
	kfree(pbt_buf);

	return ret;
}

/*
 * param - fw : const struct firmware *fw
 *
 * param - fw_ver : buffer to receive firmware version
 *
 * return : if success, return 0, otherwize return error code
 */
int focal_get_img_file_version(
	const struct firmware *fw,
	u8 *fw_ver)
{
	int fw_len = 0;
	const u8 *fw_data = NULL;

	if (NULL == fw || NULL == fw->data) {
		TS_LOG_ERR("%s:fw or fw data is null.\n", __func__);
		return -ENODATA;
	}

	fw_data = fw->data;
	fw_len = fw->size;

	if (fw_len <= FTS_FW_IMG_ADDR_VER) {
		TS_LOG_ERR("%s:fw length is to long, len=%d.\n",
			__func__, fw_len);
		return -ENOMEM;
	}

	*fw_ver = fw_data[fw_len - 2];
	return 0;
}

/*
 * param - version : buffer to receive version
 *
 * return : if need to update firmware, return 1, else return 0
 */
int focal_get_ic_firmware_version(u8 *version)
{
	int ret = 0;
	u8 fw_ver = 0;

	if (NULL == version) {
		TS_LOG_ERR("%s:version is null\n", __func__);
		return -ENOMEM;
	}

	ret = focal_read_reg(FTS_REG_FW_VER, &fw_ver);
	if (ret < 0) {
		TS_LOG_ERR("%s:read firmware version in ic fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	TS_LOG_INFO("%s:fw version in ic is:0x%x\n", __func__, fw_ver);

	*version = fw_ver;

	return 0;
}

/*
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - product_name : struct focal_platform_data *focal_pdata
 *
 * param - fw_name : struct focal_platform_data *focal_pdata
 *
 * param - size : size of fw_name
 *
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_get_firmware_name(
	struct focal_platform_data *focal_pdata,
	const char *product_name,
	char *fw_name,
	size_t size)
{
	int ret = 0;
	char vendor_name[FTS_VENDOR_NAME_LEN] = {0};
	char project_id[FTS_PROJECT_ID_LEN] = {0};
	
	strncpy(vendor_name, focal_pdata->vendor_name, FTS_VENDOR_NAME_LEN - 1);
	strncpy(project_id, focal_pdata->project_id, FTS_PROJECT_ID_LEN - 1);

	focal_strtolow(project_id, FTS_PROJECT_ID_LEN);

	ret = snprintf(fw_name, size, "ts/%s%s_%s.img",
	//ret = snprintf(fw_name, size, "%s%s_%s.img",
		product_name, project_id, vendor_name);
	if (ret >= size) {
		TS_LOG_ERR("%s:fw name buffer out of range, ret=%d\n",
			__func__, ret);
		return -ENOMEM;
	}

	ret = snprintf(module_ini_name, size, "ts/%s%s_%s.ini",
				product_name, project_id, vendor_name);
	if (ret >= size) {
		TS_LOG_ERR("%s:ini name buffer out of range, ret=%d\n",
			__func__, ret);
		return -ENOMEM;
	}

	TS_LOG_ERR("%s:fw name:%s\n", __func__, fw_name);
	TS_LOG_ERR("%s:ini name:%s\n", __func__, module_ini_name);
	return 0;
}

static int focal_get_data_check_sum(
	const u8 *data,
	size_t data_size,
	u8 *check_sum)
{
	int i = 0;

	if (NULL == check_sum || NULL == data) {
		TS_LOG_ERR("%s:input parameter is null\n", __func__);
		return -ENODATA;
	}

	/* get check sum */
	for (i = 0; i < data_size; i++)
		*check_sum ^= data[i];

	return 0;
}

static int focal_read_check_sum_in_pram(
	struct focal_platform_data *focal_pdata,
	u32 start_addr,
	u32 crc_length,
	u8 *check_sum)
{
	int ret = 0;

	ret = focal_enter_pram_model(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:enter pram model fail, ret=%d\n", __func__, ret);
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:enter pram model success\n", __func__);
	}

	ret = focal_auto_config_clock();
	if (ret) {
		TS_LOG_ERR("%s:config clock fail, ret=%d\n", __func__, ret);
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:config clock success\n", __func__);
	}

	ret = focal_enter_pram_update_model_from_pram();
	if (ret) {
		TS_LOG_ERR("%s:enter update model fail, ret=%d\n",
			__func__, ret);
		goto enter_work_model_from_pram;
	} else {
		TS_LOG_INFO("%s:enter update model success\n", __func__);
	}

	ret = focal_read_check_sum(focal_pdata, FTS_FW_IC_ADDR_START,
		crc_length, check_sum);
	if (ret) {
		TS_LOG_ERR("%s:read check sum in ic fail, ret=%d\n",
			__func__, ret);
		goto enter_work_model_from_update_model;
	}

enter_work_model_from_update_model:
	if (focal_enter_work_model_form_pram_update(focal_pdata)) {
		TS_LOG_ERR("%s:enter work model fail, ret=%d\n", __func__, ret);
	} else {
		TS_LOG_INFO("%s:enter work model success\n", __func__);
		return ret;
	}

enter_work_model_from_pram:
	/* 6. enter work model */
	if (focal_enter_work_model_from_pram(focal_pdata)) {
		TS_LOG_ERR("%s:enter work mode from pram fail, ret=%d\n",
			__func__, ret);
	} else {
		TS_LOG_INFO("%s:enter work mode from pram success\n", __func__);
	}

	return ret;
}
static int focal_get_ffbm_mode(void)
{
	char *cmdline_tp=NULL;

        cmdline_tp = strstr(saved_command_line, "androidboot.mode=ffbm");
        if(cmdline_tp != NULL)
        {
		TS_LOG_INFO("%s: ffbm mode \n", __func__);
		return 1;/* factory mode*/
        }

       TS_LOG_INFO("%s: no ffbm mode \n", __func__);
       return 0;
}
/*
 * param - fw : const struct firmware *fw
 *
 * return : if need to update firmware, return 1, else return 0
 */
static int focal_check_firmware_version(
	struct focal_platform_data *focal_pdata,
	const struct firmware *fw)
{
	int ret = 0;
	char fw_ver_in_ic = 0;
	char fw_ver_in_file = 0;

	ret = focal_get_ic_firmware_version(&fw_ver_in_ic);
	/* if get ic firmware version fail, update firmware direct */
	if (ret || fw_ver_in_ic == 0xEF||fw_ver_in_ic == 0) {
		TS_LOG_INFO("%s:%s, ret=%d, fw_version_in_ic=%d\n", __func__,
			"firmware in ic is damaged, update firmware",
			ret, fw_ver_in_ic);
		return 1;
	}

	TS_LOG_INFO("%s:firmware version in ic is:%d\n",
		__func__, fw_ver_in_ic);

	if(focal_get_ffbm_mode())
		return 0;

	ret = focal_get_img_file_version(fw, &fw_ver_in_file);
	/* do not update firmware, because firmware file may be damaged */
	if (ret) {
		TS_LOG_INFO("%s:%s, don't update firmware, ret=%d\n",
			__func__, "firmware file is damaged", ret);
		return 0;
	}

	TS_LOG_INFO("%s:firmware version in file is:%d\n",
		__func__, fw_ver_in_file);

	/* update EACH module */
	if(strstr(focal_pdata->project_id, FTS_EACH_PROJECT_ID))//each
	{
		TS_LOG_INFO("%s:%s, update firmware\n",
			__func__, "update each module firmware");
		return 1;
	}

	/* if firmware version is different, update */
	if (fw_ver_in_file > fw_ver_in_ic)
	{
		TS_LOG_INFO("%s:%s, update firmware\n",
			__func__, "firmware version is different");
		return 1;
	} else {
		TS_LOG_INFO("%s:%s, not update firmware\n",
			__func__, "firmware version is same");
	}

	return 0;
}


	
/*
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - product_name : any string is ok,
 *                        this string will be used to create firmware file name,
 *                        how to create firmeare file name please see function:
 *                        focal_get_firmware_name
 *
 * return: if read project id success, return 0, else return error code
 */
int focal_firmware_auto_update(
	struct focal_platform_data *focal_pdata,
	const char *product_name)
{
	int ret = 0;
	u8 fw_ver = 0;
	char fw_name[FTS_FW_NAME_LEN] = {0};

	struct device *dev = NULL;
	const struct firmware *fw = NULL;

	dev = &focal_pdata->focal_platform_dev->dev;

	if(true == g_focal_dev_data->need_wd_check_status){
		focal_esdcheck_set_upgrade_flag(true);
	}

	/* 1. get firmware name */
	ret = focal_get_firmware_name(focal_pdata,
		product_name, fw_name, FTS_FW_NAME_LEN);
	if (ret) {
		TS_LOG_ERR("%s:get firmware name fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	/* 3. request firmware */
	ret = request_firmware(&fw, fw_name, dev);
	if (ret != 0) {
		TS_LOG_ERR("%s:firmware request fail, ret=%d, fw_name=%s\n",
			__func__, ret, fw_name);
		return ret;
	}

	/* 4. compare firmware version */
	ret = focal_check_firmware_version(focal_pdata, fw);
	if (!ret) {
		TS_LOG_INFO("%s:no need to update firmware\n", __func__);
		goto release_fw;
	} else {
		TS_LOG_INFO("%s:going to update firmware\n", __func__);
	}

	/* 5. firmware update */
	ret = focal_firmware_update(focal_pdata, fw->data, fw->size);
	if (ret) {
		TS_LOG_ERR("%s:firmware update fail, ret=%d\n",	__func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
			if (!dsm_client_ocuppy(ts_dclient)) {
					dsm_client_record(ts_dclient,
							  "fw update result: failed.\nupdata_status is %d.\n",
							 g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status);
					dsm_client_notify(ts_dclient,DSM_TP_FWUPDATE_ERROR_NO);
			}
			strncpy(g_focal_dev_data->ts_platform_data->dsm_info.fw_update_result,
					"failed", strlen("failed"));
#endif
		goto release_fw;
	}

	/* 6. get ic firmware version */
	ret = focal_get_ic_firmware_version(&fw_ver);
	if (ret) {
		TS_LOG_ERR("%s:get firmware version fail, ret=%d\n",
			__func__, ret);
		goto release_fw;
	} else {
		focal_pdata->fw_ver = fw_ver;
	}

release_fw:
	/* 7. release firmware */
	release_firmware(fw);
	fw = NULL;

	if(true == g_focal_dev_data->need_wd_check_status){
		focal_esdcheck_set_upgrade_flag(false);
	}

	return ret;
}

/*
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - fw_name : frimware name, use this function to update firmware,
 *                   you should put the firmware file to /system/etc/firmware/
 *
 * return: if read project id success, return 0, else return error code
 */
int focal_firmware_manual_update(
	struct focal_platform_data *focal_pdata,
	const char *fw_name)
{
	int ret = 0;
	u8 fw_ver = 0;
	struct device *dev = NULL;
	const struct firmware *fw = NULL;

	dev = &focal_pdata->focal_platform_dev->dev;
	TS_LOG_DEBUG("Enter %s\n",__func__);
	/* 1. request firmware */
	ret = request_firmware(&fw, fw_name, dev);
	if (ret != 0) {
		TS_LOG_ERR("%s:firmware request fail, ret=%d, fw_name=%s\n",
			__func__, ret, fw_name);
		return ret;
	}

	/* 2. firmware update */
	ret = focal_firmware_update(focal_pdata, fw->data, fw->size);
	if (ret) {
		TS_LOG_ERR("%s:firmware update fail, ret=%d\n",	__func__, ret);
#if defined (CONFIG_HUAWEI_DSM)
			if (!dsm_client_ocuppy(ts_dclient)) {
					dsm_client_record(ts_dclient,
							  "fw update result: failed.\nupdata_status is %d.\n",
							 g_focal_dev_data->ts_platform_data->dsm_info.constraints_UPDATE_status);
					dsm_client_notify(ts_dclient,DSM_TP_FWUPDATE_ERROR_NO);
			}
			strncpy(g_focal_dev_data->ts_platform_data->dsm_info.fw_update_result,
					"failed", strlen("failed"));
#endif
		goto release_fw;
	}

	/* 3. get ic firmware version */
	ret = focal_get_ic_firmware_version(&fw_ver);
	if (ret) {
		TS_LOG_ERR("%s:get firmware version fail, ret=%d\n",
			__func__, ret);
		goto release_fw;
	} else {
		focal_pdata->fw_ver = fw_ver;
	}

	/* 4. set hardware info */
	ret = focal_hardwareinfo_set();
	if (ret < 0){
		TS_LOG_ERR("%s:hardwareinfo_set error, ret=%d\n", __func__, ret);
		goto release_fw;
	}

release_fw:
	/* 5. release firmware */
	release_firmware(fw);
	fw = NULL;

	return ret;
}

static int focal_read_vendor_id_in_pram(u8 *vendor_id)
{
	int ret = 0;
	u8 reg_val = 0;
	u8 cmd[4] = {0};

	cmd[0] = FTS_CMD_READ_FLASH;
	cmd[1] = 0x00;
	cmd[2] = FTS_BOOT_VENDER_ID_ADDR1;
	cmd[3] = FTS_BOOT_VENDER_ID_ADDR2;

	ret = focal_write(cmd, 4);
	if (ret) {
		TS_LOG_ERR("%s:vendor id cmd write fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	msleep(1);

	ret = focal_read(cmd, 4, &reg_val, 1);
	if (ret) {
		TS_LOG_ERR("%s:vendor id read fail, ret=%d\n", __func__, ret);
		return ret;
	}

	if (0 == reg_val) {
		TS_LOG_ERR("%s:vendor id read fail\n", __func__);
		return -EINVAL;
	}

	*vendor_id = reg_val;

	return 0;
}

int focal_read_vendor_id_from_app(u8 *vendor_id)
{
	int ret = 0;

	u8 cmd = 0;
	u8 reg_val = 0;

	cmd = FTS_REG_VENDOR_ID;
	ret = focal_read_reg(cmd, &reg_val);
	if (ret) {
		TS_LOG_ERR("%s:read vendor id fail, ret=%d\n", __func__, ret);
		return ret;
	}

	*vendor_id = reg_val;
	return 0;
}

/*
 * param - focal_pdata : struct focal_platform_data *focal_pdata
 *
 * param - vendor_id : buffer to recevice vendor id
 *
 * return: if read project id success, return 0, else return error code
 */
int focal_read_vendor_id_from_pram(
	struct focal_platform_data *focal_pdata,
	u8 *vendor_id)
{
	int i = 0;
	int ret = 0;

	ret = focal_enter_pram_model(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:write pram fail, ret=%d\n", __func__, ret);
		goto exit;
	}

	ret = focal_auto_config_clock();
	if (ret) {
		TS_LOG_ERR("%s:auto config clock fail, ret = %d\n",
			__func__, ret);
		goto exit;
	}

	ret = focal_enter_pram_update_model_from_pram();
	if (ret) {
		TS_LOG_ERR("%s:enter pram update model fail, ret = %d\n",
			__func__, ret);
		goto exit;
	}

	for (i = 0; i < focal_pdata->delay_time->upgrade_loop_times; i++) {
		ret = focal_read_vendor_id_in_pram(vendor_id);
		if (!ret) {
			break;
		} else {
			TS_LOG_INFO("%s:%s, ret=%d, retry=%d\n",
				__func__, "read vendor id fail", ret, i);
			msleep(5);
		}
	}

exit:
	focal_enter_work_model_from_rom_update(focal_pdata);
	focal_enter_work_model_by_hardware();

	return ret;
}

int focal_read_vendor_id(
	struct focal_platform_data *focal_pdata,
	u8 *vendor_id)
{
	int ret = 0;

	ret = focal_read_vendor_id_from_app(vendor_id);
	if (!ret)
		return 0;

	TS_LOG_INFO("%s:%s, ret=%d\n", __func__,
		"read vendor id from app fail, try to read from pram", ret);
	ret = focal_read_vendor_id_from_pram(focal_pdata, vendor_id);
	if (!ret)
		return 0;

	TS_LOG_ERR("%s:read vendor id fail, ret=%d\n", __func__, ret);

	return ret;
}


/*
 * param - offset : addr in ic to start read pram
 *
 * param - buf : buffer to recevice pram data
 *
 * param - size : data size will be read
 *
 * return: if read project id success, return 0, else return error code
 */
bool focal_read_pram_package(
	u32 offset,
	u8 *buf,
	u16 size)
{
	int ret = -1;
	u8 command[4];

	command[0] = FTS_CMD_READ_PRAM;
	command[1] = 0x00;
	command[2] = offset >> 8;
	command[3] = offset;

	ret = focal_read(command, 4, buf, size);
	if (ret < 0) {
		TS_LOG_ERR("%s:read pram package fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}


/*
 * param - chip_id : buffer to receive chip id
 *
 * return: if read project id success, return 0, else return error code
 */
int focal_read_chip_id_(u32 *chip_id)
{
	int ret = 0;
	u8 cmd[4] = {0};
	u8 reg_val[2] = {0, 0};

	if (NULL == chip_id)
		return -ENOMEM;

	cmd[0] = FTS_REG_BOOT_CHIP_ID;
	cmd[1] = 0x00;
	cmd[2] = 0x00;
	cmd[3] = 0x00;

	ret = focal_read(cmd, 4, reg_val, 2);
	if (ret) {
		TS_LOG_ERR("%s:read chip id fail, ret=%d\n", __func__, ret);
		return ret;
	}

	*chip_id = (reg_val[0] << 8) | reg_val[1];
	if((reg_val[0] == 0xef)||(reg_val[1] == 0xef))
	{
		return -ENOMEM;
	}

	return 0;
}

/*
 * param - focal_pdata : struct focal_platform_data * focal_pdata
 *
 * param - chip_id   : buffer to receive chip id
 *
 * return: if read project id success, return 0, else return error code
 */
int focal_read_chip_id_from_rom(
	struct focal_platform_data *focal_pdata,
	u32 *chip_id)
{
	int ret = 0;
	ret = focal_enter_rom_update_model_by_hardware(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:enter bootloader fail, ret=%d\n", __func__, ret);
		return ret;
	}
	ret = focal_enter_work_model_from_rom_update(focal_pdata);

	return 0;
}

int focal_read_chip_id_from_app(u32 *chip_id)
{
	int ret = 0;

	u8 cmd = 0;
	u8 reg_val[2] = {0};

	cmd = FTS_REG_CHIP_ID_H;
	ret = focal_read(&cmd, 1, &reg_val[0], 1);
	if (ret) {
		TS_LOG_ERR("%s:read chip id H fail, ret=%d\n", __func__, ret);
		return ret;
	}

	cmd = FTS_REG_CHIP_ID_L;
	ret = focal_read(&cmd, 1, &reg_val[1], 1);
	if (ret) {
		TS_LOG_ERR("%s:read chip id L fail, ret=%d\n", __func__, ret);
		return ret;
	}

	*chip_id = reg_val[0] << 8 | reg_val[1];

	return 0;
}

int focal_read_chip_id(
	struct focal_platform_data *focal_pdata,
	u32 *chip_id)
{
	int ret = 0;

	ret = focal_read_chip_id_from_app(chip_id);
	if (!ret) {
		return 0;
	}

	TS_LOG_INFO("%s:read chip id from app fail, try to read from rom\n",
		__func__);
	ret = focal_read_chip_id_from_rom(focal_pdata, chip_id);
	if (!ret)
		return 0;

	TS_LOG_ERR("%s:read chip id fail, ret=%d\n", __func__, ret);
	return ret;
}

/*
 * return : if read project id success, return 0, else return error code
 *
 * notice : this function is not dependable,
 *          because the value of FTS_WORK_MODE_VALUE is 0,
 *          if read work model fail, the value is also 0,
 *          so we can not guarantee than the ic is in work model
 */
int focal_enter_work_model_by_software(void)
{
	int i = 0;
	int ret = 0;
	u8 work_mode = 0;

	for (i = 0; i < FTS_MODEL_SET_NO; i++) {
		/* read ic run mode */
		ret = focal_read_reg(FTS_WORK_MODE_ADDR, &work_mode);
		if (ret) {
			TS_LOG_ERR("%s:read work model fail, ret=%d\n",
				__func__, ret);
			return ret;
		}

		/* check if ic is in work mode */
		if (FTS_WORK_MODE_VALUE == work_mode)
			return 0;

		if (i + 1 == FTS_MODEL_SET_NO)
			break;

		/* set to work mode */
		ret = focal_write_reg(FTS_WORK_MODE_ADDR, FTS_WORK_MODE_VALUE);
		if (ret) {
			TS_LOG_ERR("%s:write work mode fail, ret=%d\n",
				__func__, ret);
			return ret;
		}

		msleep(20);
	}

	return -ENODEV;
}

/*
 * return: if read project id success, return 0, else return error code
 */
static int focal_enter_work_model_by_hardware(void)
{
	int ret = 0;
	u8 work_mode = 0;

	ret = focal_hardware_reset_to_normal_model();
	if (ret) {
		TS_LOG_ERR("%s: hardware reset fail, ret=%d\n", __func__, ret);
		return -ENODEV;
	}

	/* check again */
	ret = focal_read_reg(FTS_WORK_MODE_ADDR, &work_mode);
	if (ret < 0) {
		TS_LOG_ERR("%s:enter work mode fail, ret=%d\n", __func__, ret);
		return -EIO;
	}

	if (FTS_WORK_MODE_VALUE != work_mode) {
		TS_LOG_ERR("%s:enter work mode fail, mode=%d\n",
			__func__, work_mode);
		return -EINVAL;
	}

	TS_LOG_INFO("%s:enter work mode success\n", __func__);

	return 0;
}

/*
 * return: if read project id success, return 0, else return error code
 *
 * notice : this function is not dependable,
 *          because the value of FTS_WORK_MODE_VALUE is 0,
 *          if read work model fail, the value is also 0,
 *          so we can not guarantee than the ic is in work model
 */
int focal_enter_work_model(void)
{
	int ret = 0;
	ret = focal_enter_work_model_by_software();
	if (ret) {
		TS_LOG_INFO("%s:%s\n", __func__,
			"enter work mode by software fail, try hardware.");
		ret = focal_enter_work_model_by_hardware();
	}

	return ret;
}

/*
 * return: if read project id success, return 0, else return error code
 */
int focal_enter_work_model_form_pram_update(struct focal_platform_data *focal_pdata)
{
	int ret = 0;
	u8 cmd = 0x00;

	cmd = FTS_CMD_REBOOT_APP;
	ret = focal_write(&cmd, 1);
	if (ret) {
		TS_LOG_ERR("%s:write reboot app command fail, ret = %d\n",
			__func__, ret);
		return ret;
	}

	msleep(focal_pdata->delay_time->reboot_delay);
	return 0;
}

/*
 * return: if read project id success, return 0, else return error code
 */
static int focal_enter_work_model_from_rom_update(
	struct focal_platform_data *focal_pdata)
{
	int ret = 0;
	u8 cmd = 0x00;

	/* this command tell ic shift from pram tp app */
	cmd = FTS_REG_RESET_FW;
	ret = focal_write(&cmd, 1);
	if (ret) {
		TS_LOG_ERR("%s:reset command send fail, ret = %d\n",
			__func__, ret);
		return ret;
	}

	/* make sure CTP startup normally */
	msleep(focal_pdata->delay_time->reboot_delay);

	ret = focal_enter_work_model();

	return ret;
}

/*
 * return: if read project id success, return 0, else return error code
 */
static int focal_enter_work_model_from_pram(struct focal_platform_data *focal_pdata)
{
	return focal_enter_work_model_from_rom_update(focal_pdata);
}

int focal_read_project_id_from_app(u8 *project_id_out, size_t size)
{
	int ret = 0;
	u8 cmd[2] = {0};
	u8 project_id[FTS_PROJECT_ID_LEN] = {0};

	cmd[0] = FTS_REG_OFFSET_CODE;
	cmd[1] = 0x20;
	ret = focal_write(cmd, 2);
	if (ret) {
		TS_LOG_ERR("%s:write offset code fail, ret=%d\n",
			__func__, ret);
		return ret;
	}

	cmd[0] = FTS_REG_PROJ_CODE;
	ret = focal_read(cmd, 1, project_id, FTS_PROJECT_ID_LEN);
	if (ret < 0) {
		TS_LOG_ERR("%s:read app project id error, ret=%d\n",
			__func__, ret);
		return ret;
	} else {
		strncpy(project_id_out, project_id, size);
		TS_LOG_INFO("%s:read project id:%s\n", __func__, project_id);
	}

	return 0;
}

/*
 * name: focal_read_project_id_from_pram
 *
 * param: project_id - buffer to receive project id
 *
 * param: size       - size of project_id
 *
 * return: if read project id success, return 0, else return error code
 *
 * notice: this function may made ic reset
 */
int focal_read_project_id_from_pram(
	struct focal_platform_data *focal_pdata,
	char *project_id,
	size_t size)
{
	int ret = 0;
	int is_read_project_id_success = -EINVAL;

	u32 i = 0;
	u8 cmd[4] = {0};
	u8 reg_val[FTS_PROJECT_ID_LEN] = {0};

	/* 1. enter pram model */
	ret = focal_enter_pram_model(focal_pdata);
	if (ret) {
		TS_LOG_ERR("%s:write pram fail ret = %d\n", __func__, ret);
		goto out;
	} else {
		TS_LOG_INFO("%s:wirte pram success\n", __func__);
	}

	/* 2. config click */
	ret = focal_auto_config_clock();
	if (ret) {
		TS_LOG_ERR("%s:config click fail, ret = %d\n", __func__, ret);
		goto out;
	} else {
		TS_LOG_INFO("%s:config click success\n", __func__);
	}

	/* 3. enter pramboot upgrade mode */
	ret = focal_enter_pram_update_model_from_pram();
	if (ret) {
		TS_LOG_ERR("%s:enter update mode fail, ret=%d\n",
			__func__, ret);
		goto out;
	} else {
		TS_LOG_INFO("%s:enter update mode success\n", __func__);
	}

	cmd[0] = FTS_CMD_READ_FLASH;
	cmd[1] = 0x00;
	cmd[2] = (focal_pdata->pram_projectid_addr & 0x0FFFF) >>8;
	cmd[3] = focal_pdata->pram_projectid_addr & 0x0FF;
	TS_LOG_INFO("%s:cmd[2] = %x, cmd[3] =%x\n", __func__,cmd[2],cmd[3]);
//	cmd[2] = FTS_BOOT_PROJ_CODE_ADDR1;
//	cmd[3] = FTS_BOOT_PROJ_CODE_ADDR2;
	for (i = 0; i < focal_pdata->delay_time->upgrade_loop_times; i++) {
		reg_val[0] = reg_val[1] = 0x00;
		ret = focal_write(cmd, 4);
		if (ret) {
			TS_LOG_ERR("%s:%s, ret=%d, retry=%d\n", __func__,
				"write project id cmd fail", ret, i);
			msleep(5);
			continue;
		}
		msleep(1);
		ret = focal_read_default(reg_val, FTS_PROJECT_ID_LEN);
		if (ret) {
			TS_LOG_ERR("%s:%s, ret=%d, retry=%d\n", __func__,
				"read project id fail", ret, i);
			msleep(5);
			continue;
		}
		if (0 == reg_val[0]) {
			TS_LOG_INFO("%s:read project id fail, retry=%d\n",
				__func__, i);
			msleep(5);
			continue;
		} else {
			memcpy(project_id, reg_val, size);
			is_read_project_id_success = 0;
			break;
		}
	}

	if (is_read_project_id_success == 0)
		TS_LOG_INFO("%s:read project id success\n", __func__);
	else
		TS_LOG_ERR("%s:read project id fail\n", __func__);

out:
	focal_enter_work_model_from_rom_update(focal_pdata);
	focal_enter_work_model_by_hardware();

	return is_read_project_id_success;
}

#define READ_PROJECT_FROM_APP
int focal_read_project_id(
	struct focal_platform_data *focal_pdata,
	char *project_id,
	size_t size)
{
	int ret = NO_ERR;

	memset(project_id, 0, size);
#if 0
//read project ID from fw  cannot be trusted when the fw is damaged,
#ifdef READ_PROJECT_FROM_APP
	ret = focal_read_project_id_from_app(project_id, size);
	if (!ret && strlen(project_id) > 0) {
		TS_LOG_INFO("%s:read project id from app success\n", __func__);
		return 0;
	}
#endif /* READ_PROJECT_FROM_APP */
#endif
	ret = focal_read_project_id_from_pram(focal_pdata, project_id, size);
	if (ret) {
		TS_LOG_ERR("%s:read project id from pram fail, ret=%d\n",
			__func__, ret);
		return -EINVAL;
	} else {
		TS_LOG_INFO("%s:read project id from pram success\n", __func__);
		return 0;
	}
}

int focal_flash_init(struct i2c_client *client)
{
	return NO_ERR;
}

int focal_flash_exit(void)
{
	return NO_ERR;
}
