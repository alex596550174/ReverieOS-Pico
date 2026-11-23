// SPDX-License-Identifier: Apache-2.0
/* Independently authored SD ownership and USB MSC integration. TinyUSB and
   ESP-IDF provide the unchanged USB/SD hardware drivers. Never mount FAT here. */
#include "board.h"
#include "thingsos/usb_disk.h"
#include "tusb.h"
#include "esp_private/usb_phy.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "read_pico_board.h"
#include "read_pico_sd.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static sdmmc_card_t* card;
static uint8_t* dma_sector;
static size_t dma_cap;
static int64_t next_card_check;
static SemaphoreHandle_t access, stopped;
static usb_phy_handle_t phy;
static volatile bool running, mounted;
static int64_t disconnected_at;
static unsigned reads, writes;
static esp_err_t last_error;
static thingsos_usb_disk_t disk;
static char serial_number[13];

static const tusb_desc_device_t device_descriptor={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x0200,.bMaxPacketSize0=64,.idVendor=0x303a,.idProduct=0x4002,
    .bcdDevice=0x0402,.iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1
};
static const uint8_t configuration[]={
    TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_MSC_DESC_LEN,0,100),
    TUD_MSC_DESCRIPTOR(0,0,0x01,0x81,64)
};
uint8_t const* tud_descriptor_device_cb(void){return (const uint8_t*)&device_descriptor;}
uint8_t const* tud_descriptor_configuration_cb(uint8_t index){(void)index;return configuration;}
uint16_t const* tud_descriptor_string_cb(uint8_t index,uint16_t language) {
    (void)language;static uint16_t out[40];
    if(!index){out[0]=(TUSB_DESC_STRING<<8)|4;out[1]=0x0409;return out;}
    const char* text=index==1?"ThingsOS":index==2?"ThingsOS SD Card":index==3?serial_number:NULL;
    if(!text)return NULL;
    unsigned n=(unsigned)strlen(text);if(n>39)n=39;
    out[0]=(uint16_t)((TUSB_DESC_STRING<<8)|(2*n+2));
    for(unsigned i=0;i<n;++i)out[i+1]=(uint8_t)text[i];
    return out;
}
void tud_mount_cb(void){xSemaphoreTake(access,portMAX_DELAY);mounted=true;xSemaphoreGive(access);}
void tud_umount_cb(void){xSemaphoreTake(access,portMAX_DELAY);mounted=false;disconnected_at=esp_timer_get_time();xSemaphoreGive(access);}
void tud_suspend_cb(bool remote_wakeup){(void)remote_wakeup;}
void tud_resume_cb(void){}
void tud_msc_inquiry_cb(uint8_t lun,uint8_t vendor[8],uint8_t product[16],uint8_t revision[4]) {
    (void)lun;memcpy(vendor,"ThingsOS",8);memcpy(product,"SD Card         ",16);memcpy(revision,"0302",4);
}
/* Called with access held. A single failed I2C read isn't a removal. */
static bool usb_sd_present_debounced(void) {
    bool present=false;
    for(unsigned sample=0;sample<3;++sample) {
        if(sample)vTaskDelay(pdMS_TO_TICKS(5));
        if(read_pico_sd_present())present=true;
    }
    return present;
}
static void usb_sd_check_removal(void) {
    next_card_check=esp_timer_get_time()+500000;
    if(!usb_sd_present_debounced()) {
        disk.enabled=false;
        last_error=ESP_ERR_NOT_FOUND;
    }
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    xSemaphoreTake(access,portMAX_DELAY);
    if(disk.enabled && !disk.ejected && esp_timer_get_time()>=next_card_check)usb_sd_check_removal();
    bool ready=disk.enabled && !disk.ejected;
    xSemaphoreGive(access);
    if(!ready)tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);
    return ready;
}
void tud_msc_capacity_cb(uint8_t lun,uint32_t* count,uint16_t* size) {
    (void)lun;*count=(uint32_t)disk.sectors;*size=512;
}
bool tud_msc_is_writable_cb(uint8_t lun){return tud_msc_test_unit_ready_cb(lun);}
bool tud_msc_start_stop_cb(uint8_t lun,uint8_t power,bool start,bool load_eject) {
    (void)lun;(void)power;
    xSemaphoreTake(access,portMAX_DELAY);
    if(load_eject)disk.ejected=!start;
    xSemaphoreGive(access);return true;
}
/* Both the aligned fast path and partial-sector fallback own access. */
static bool sd_transfer(bool write, uint64_t lba, size_t sectors) {
    for(unsigned attempt=0;attempt<2;++attempt) {
        if(attempt)vTaskDelay(pdMS_TO_TICKS(5));
        last_error=write?sdmmc_write_sectors(card,dma_sector,(size_t)lba,sectors)
                        :sdmmc_read_sectors(card,dma_sector,(size_t)lba,sectors);
        if(last_error==ESP_OK) {
            if(write)writes+=(unsigned)sectors;
            else reads+=(unsigned)sectors;
            return true;
        }
    }
    usb_sd_check_removal();
    return false;
}
static bool read_sector(void* context,uint64_t lba,void* out) {
    (void)context;
    if(!sd_transfer(false,lba,1))return false;
    memcpy(out,dma_sector,THINGSOS_USB_SECTOR);
    return true;
}
static bool write_sector(void* context,uint64_t lba,const void* in) {
    (void)context;
    memcpy(dma_sector,in,THINGSOS_USB_SECTOR);
    return sd_transfer(true,lba,1);
}
static int32_t transfer(uint8_t lun,bool write,uint32_t lba,uint32_t offset,void* buffer,uint32_t bytes) {
    xSemaphoreTake(access,portMAX_DELAY);
    bool ok=false;
    if(!offset && bytes && bytes%THINGSOS_USB_SECTOR==0 && bytes<=dma_cap) {
        size_t sectors=bytes/THINGSOS_USB_SECTOR;
        if(buffer && disk.enabled && !disk.ejected && lba<disk.sectors && sectors<=disk.sectors-lba) {
            if(write)memcpy(dma_sector,buffer,bytes);
            ok=sd_transfer(write,lba,sectors);
            if(ok && !write)memcpy(buffer,dma_sector,bytes);
        }
    } else ok=thingsos_usb_disk_io(&disk,write,lba,offset,buffer,bytes);
    xSemaphoreGive(access);
    if(!ok){tud_msc_set_sense(lun,SCSI_SENSE_MEDIUM_ERROR,write?0x0c:0x11,0);return -1;}
    return (int32_t)bytes;
}
int32_t tud_msc_read10_cb(uint8_t lun,uint32_t lba,uint32_t offset,void* buffer,uint32_t bytes) {
    return transfer(lun,false,lba,offset,buffer,bytes);
}
int32_t tud_msc_write10_cb(uint8_t lun,uint32_t lba,uint32_t offset,uint8_t* buffer,uint32_t bytes) {
    return transfer(lun,true,lba,offset,buffer,bytes);
}
int32_t tud_msc_scsi_cb(uint8_t lun,const uint8_t command[16],void* buffer,uint16_t bytes) {
    (void)buffer;(void)bytes;
    /* Raw writes are synchronous; no device-side filesystem cache exists. */
    if(command[0]==0x35 || command[0]==0x1e)return 0; /* SYNCHRONIZE CACHE / PREVENT ALLOW MEDIUM REMOVAL */
    tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x20,0);return -1;
}
static void usb_task(void* unused) {
    (void)unused;
    while(running)tud_task_ext(20,false);
    xSemaphoreGive(stopped);vTaskDelete(NULL);
}
esp_err_t thingsos_board_usb_start(void) {
    if(running)return ESP_ERR_INVALID_STATE;
    read_pico_sd_info_t info={0};
    if(read_pico_sd_get_info(&info)==ESP_ERR_NOT_FINISHED || info.mounted || !read_pico_sd_present())return ESP_ERR_INVALID_STATE;
    access=xSemaphoreCreateMutex();stopped=xSemaphoreCreateBinary();
    dma_cap=CFG_TUD_MSC_EP_BUFSIZE;
    dma_sector=heap_caps_malloc(dma_cap,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!dma_sector) {
        dma_cap=4096;
        dma_sector=heap_caps_malloc(dma_cap,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    }
    if(!dma_sector)dma_cap=0;
    card=calloc(1,sizeof(*card));
    if(!access || !stopped || !dma_sector || !card)return ESP_ERR_NO_MEM;
    sdmmc_host_t host=SDMMC_HOST_DEFAULT();host.max_freq_khz=SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width=1;slot.clk=GPIO_NUM_38;slot.cmd=GPIO_NUM_42;slot.d0=GPIO_NUM_44;
    slot.d1=slot.d2=slot.d3=slot.cd=slot.wp=GPIO_NUM_NC;
    slot.flags|=SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e=sdmmc_host_init();
    if(e==ESP_OK)e=sdmmc_host_init_slot(host.slot,&slot);
    if(e==ESP_OK)e=sdmmc_card_init(&host,card);
    if(e!=ESP_OK)return e;
    if(card->csd.sector_size!=512 || !card->csd.capacity)return ESP_ERR_INVALID_SIZE;
    disk=(thingsos_usb_disk_t){card->csd.capacity,true,false,NULL,read_sector,write_sector};
    uint8_t mac[6];esp_efuse_mac_get_default(mac);
    snprintf(serial_number,sizeof(serial_number),"%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    /* Serial/JTAG and USB OTG share the internal PHY. Do not log to the old
       console after handing the PHY to MSC. Reboot restores normal console. */
    esp_log_level_set("*",ESP_LOG_NONE);
    usb_phy_config_t config={.controller=USB_PHY_CTRL_OTG,.target=USB_PHY_TARGET_INT,
        .otg_mode=USB_OTG_MODE_DEVICE,.otg_speed=USB_PHY_SPEED_FULL};
    e=usb_new_phy(&config,&phy);if(e!=ESP_OK)return e;
    const tusb_rhport_init_t port={.role=TUSB_ROLE_DEVICE,.speed=TUSB_SPEED_FULL};
    if(!tusb_rhport_init(0,&port))return ESP_FAIL;
    next_card_check=0;
    running=true;mounted=false;disconnected_at=esp_timer_get_time();
    if(xTaskCreate(usb_task,"usb_sd",4096,NULL,5,NULL)!=pdPASS){running=false;return ESP_ERR_NO_MEM;}
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    tud_connect();
    return ESP_OK;
}
void thingsos_board_usb_status(thingsos_usb_status_t* out) {
    if(!out)return;
    if(access)xSemaphoreTake(access,portMAX_DELAY);
    *out=(thingsos_usb_status_t){running,mounted,disk.ejected,reads,writes,last_error,disk.sectors*512};
    if(access)xSemaphoreGive(access);
}
esp_err_t thingsos_board_usb_stop(void) {
    if(!running)return ESP_ERR_INVALID_STATE;
    /* Windows safe removal does not reliably issue START STOP UNIT or a USB
       unmount notification. The explicit device switch must still work.
       Serialize behind the current sector I/O, reject all subsequent I/O,
       disconnect the host, then stop the stack before rebuilding local FAT. */
    xSemaphoreTake(access,portMAX_DELAY);
    disk.enabled=false;disk.ejected=true;
    xSemaphoreGive(access);
    tud_disconnect();running=false;
    if(xSemaphoreTake(stopped,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    tud_deinit(0);
    /* The caller immediately restarts, rebuilding FAT, font and library
       state from the card after the host has relinquished ownership. */
    if(phy)usb_del_phy(phy);
    sdmmc_host_deinit();return ESP_OK;
}
