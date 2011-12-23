#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <reboot/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"


int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("Script Asserts: %s\n", script_assert_enabled ? "Enabled" : "Disabled");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- Installing: %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("Installation aborted.\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\nInstall from sdcard complete.\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "apply sdcard:update.zip",
                                "choose zip from sdcard",
                                "toggle signature verification",
                                "toggle script asserts",
                                NULL };
#define ITEM_APPLY_SDCARD     0
#define ITEM_CHOOSE_ZIP       1
#define ITEM_SIG_CHECK        2
#define ITEM_ASSERTS          3

void show_install_update_menu()
{
    static char* headers[] = {  "Apply update from .zip file on SD card",
                                "",
                                NULL
    };
    
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0, 0);
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_APPLY_SDCARD:
            {
                if (confirm_selection("Confirm install?", "Yes - Install /sdcard/update.zip"))
                    install_zip(SDCARD_UPDATE_FILE);
                break;
            }
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu("/sdcard/");
                break;
            default:
                return;
        }

    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("No files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point)
{
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE ("Can't mount %s\n", mount_point);
        return;
    }

    static char* headers[] = {  "Choose a zip to apply",
                                "",
                                NULL
    };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    static char* confirm_install  = "Confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(file);
}

void show_nandroid_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    static char* headers[] = {  "Choose an image to restore",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm restore?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);
}

#ifndef BOARD_UMS_LUNFILE
#define BOARD_UMS_LUNFILE	"/sys/devices/platform/usb_mass_storage/lun0/file"
#endif

void show_mount_usb_storage_menu()
{
    int fd;
    Volume *vol = volume_for_path("/sdcard");
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    if ((write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("Unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
    static char* headers[] = {  "USB Mass Storage device",
                                "Leaving this menu unmount",
                                "your SD card from your PC.",
                                "",
                                NULL
    };

    static char* list[] = { "Unmount", NULL };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    char ch = 0;
    if (write(fd, &ch, 1) < 0) {
        LOGE("Unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "  THIS CAN NOT BE UNDONE.", "", NULL };
	if (0 == stat("/sdcard/clockworkmod/.one_confirm", &info)) {
		char* items[] = { "No",
						confirm, //" Yes -- wipe partition",   // [1]
						NULL };
		int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
		return chosen_item == 1;
	}
	else {
		char* items[] = { "No",
						"No",
						"No",
						"No",
						"No",
						"No",
						"No",
						confirm, //" Yes -- wipe partition",   // [7]
						"No",
						"No",
						"No",
						NULL };
		int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
		return chosen_item == 7;
	}
	}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_device(const char *device, const char *path, const char *fs_type) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // no /sdcard? let's assume /data/media
        if (strstr(path, "/sdcard") == path && is_data_media()) {
            return format_unknown_device(NULL, path, NULL);
        }
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", path);
        return -1;
    }
    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", path);
        return -1;
    }

    if (strcmp(fs_type, "rfs") == 0) {
        if (ensure_path_unmounted(path) != 0) {
            LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
            return -1;
        }
        if (0 != format_rfs_device(device, path)) {
            LOGE("format_volume: format_rfs_device failed on %s\n", device);
            return -1;
        }
        return 0;
    }
 
    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n",device);
            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        reset_ext4fs_info();
        int result = make_ext4fs(device, NULL, NULL, 0, 0, 0);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", device);
            return -1;
        }
        return 0;
    }

    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("Formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("No app2sd partition found. Skipping format of /sd-ext.\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("Formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("Formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("Error mounting %s!\n", path);
        ui_print("Skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_partition_menu()
{
    static char* headers[] = {  "Mounts and Storage Menu",
                                "",
                                NULL
    };

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
		return;

		mountable_volumes = 0;
		formatable_volumes = 0;

		mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
		format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

		for (i = 0; i < num_volumes; ++i) {
			Volume* v = &device_volumes[i];
			if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0)
			{
				sprintf(&mount_menue[mountable_volumes].mount, "mount %s", v->mount_point);
				sprintf(&mount_menue[mountable_volumes].unmount, "unmount %s", v->mount_point);
				mount_menue[mountable_volumes].v = &device_volumes[i];
				++mountable_volumes;
				if (is_safe_to_format(v->mount_point)) {
					sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
					format_menue[formatable_volumes].v = &device_volumes[i];
					++formatable_volumes;
				}
		    }
		    else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
		    {
				sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
				format_menue[formatable_volumes].v = &device_volumes[i];
				++formatable_volumes;
			}
		}


    static char* confirm_format  = "Confirm format?";
    static char* confirm = "Yes - Format";
    char confirm_string[255];

    for (;;)
    {

		for (i = 0; i < mountable_volumes; i++)
		{
			MountMenuEntry* e = &mount_menue[i];
			Volume* v = e->v;
			if(is_path_mounted(v->mount_point))
				options[i] = e->unmount;
			else
				options[i] = e->mount;
		}

		for (i = 0; i < formatable_volumes; i++)
		{
			FormatMenuEntry* e = &format_menue[i];

			options[mountable_volumes+i] = e->txt;
		}

        options[mountable_volumes+formatable_volumes] = "mount USB storage";
        options[mountable_volumes+formatable_volumes + 1] = NULL;

        int chosen_item = get_menu_selection(headers, &options, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == (mountable_volumes+formatable_volumes))
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < mountable_volumes)
        {
			MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("Error unmounting %s!\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("Error mounting %s!\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("Formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("Error formatting %s!\n", v->mount_point);
            else
                ui_print("Done.\n");
        }
    }

    free(mount_menue);
    free(format_menue);

}

void show_nandroid_advanced_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("Can't mount sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "Choose an image to restore",
                                "",
                                "Choose an image to restore",
                                "first. The next menu will",
                                "you more options.",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "Nandroid Advanced Restore",
                                "",
                                NULL
    };

    static char* list[] = { "Restore boot",
                            "Restore system",
                            "Restore data",
                            "Restore cache",
                            "Restore sd-ext",
                            "Restore wimax",
                            NULL
    };
    
    if (0 != get_partition_device("wimax", tmp)) {
        // disable wimax restore option
        list[5] = NULL;
    }

    static char* confirm_restore  = "Confirm restore?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Restore boot"))
                nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Restore system"))
                nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Restore data"))
                nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Restore cache"))
                nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "Yes - Restore sd-ext"))
                nandroid_restore(file, 0, 0, 0, 0, 1, 0);
            break;
        case 5:
            if (confirm_selection(confirm_restore, "Yes - Restore wimax"))
                nandroid_restore(file, 0, 0, 0, 0, 0, 1);
            break;
    }
}

void show_nandroid_advanced_backup_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("Can't mount sdcard\n");
        return;
    }

    static char* headers[] = {  "Nandroid Advanced Backup",
                                "",
                                NULL
    };

    static char* list[] = { "Backup boot",
                            "Backup system",
                            "Backup data",
                            "Backup cache",
                            "Backup sd-ext",
                            NULL
    };

	char backup_path[PATH_MAX];
	time_t t = time(NULL);
	struct tm *tmp = localtime(&t);
	if (tmp == NULL)
	{
		struct timeval tp;
		gettimeofday(&tp, NULL);
		sprintf(backup_path, "%s/clockworkmod/backup/%d", path, tp.tv_sec);
	}
	else
	{
		strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
	}
	static char* confirm_restore  = "Confirm backup?";
	
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Backup boot"))
                nandroid_backup_boot(backup_path);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Backup system"))
                nandroid_backup_system(backup_path);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Backup data"))
                nandroid_backup_data(backup_path);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Backup cache"))
                nandroid_backup_cache(backup_path);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "Yes - Backup sd-ext"))
                nandroid_backup_sd(backup_path);;
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL
    };

    static char* list[] = { "~~~> Go Back <~~~",
			    "Backup",
                            "Restore",
			    "Advanced Backup",
                            "Advanced Restore",
                            NULL
    };
for (;;)
    {

    int chosen_item = get_menu_selection(headers, list, 0, 0);
	if (chosen_item == GO_BACK)
            break;
    switch (chosen_item)
    {
		case 0:
	    {
			return;
			break;
	    }
        case 1:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 2:
            show_nandroid_restore_menu("/sdcard");
            break;
        case 3:
            show_nandroid_advanced_backup_menu("/sdcard");
            break;
        case 4:
            show_nandroid_advanced_restore_menu("/sdcard");
            break;
    }
	}
}

void wipe_battery_stats()
{
    ensure_path_mounted("/data");
    remove("/data/system/batterystats.bin");
    ensure_path_unmounted("/data");
    ui_print("Battery Stats wiped.\n");
}

void show_advanced_menu()
{
    static char* headers[] = {  "Advanced and Debugging Menu",
                                "",
                                NULL
    };

    static char* list[] = {  "~~~> Go Back <~~~",
							"Reboot Recovery",
                            "Wipe Dalvik Cache",
                            "Wipe Battery Stats",
                            "Report Error",
                            "Key Test",
                            "Show log",
#ifndef BOARD_HAS_SMALL_RECOVERY
                            "Partition SD Card",
                            "Fix Permissions",
#ifdef BOARD_HAS_SDCARD_INTERNAL
                            "Partition Internal SD Card",
#endif
#endif
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
			case 0:
			{
				return;
				break;
			}
            case 1:
            {
                reboot_wrapper("recovery");
                break;
            }
            case 2:
            {
                if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("Dalvik Cache wiped.\n");
                }
                ensure_path_unmounted("/data");
                break;
            }
            case 3:
            {
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Battery Stats"))
                    wipe_battery_stats();
                break;
            }
            case 4:
                handle_failure(1);
                break;
            case 5:
            {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 6:
            {
                ui_printlogtail(12);
                break;
            }
            case 7:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Ext Size", "", NULL };
                static char* swap_headers[] = { "Swap Size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/sdcard");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("Partitioning SD Card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("Done!\n");
                else
                    ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
                break;
            }
            case 8:
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("Fixing permissions...\n");
                __system("fix_permissions");
                ui_print("Done!\n");
                break;
            }
            case 9:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Data Size", "", NULL };
                static char* swap_headers[] = { "Swap Size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int swap_size = 0;
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/emmc");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("Partitioning Internal SD Card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("Done!\n");
                else
                    ui_print("An error occured while partitioning your Internal SD Card. Please see /tmp/recovery.log for more details.\n");
                break;
            }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("Unable to get recovery.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    write_fstab_root("/datadata", file);
    write_fstab_root("/emmc", file);
    write_fstab_root("/system", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    fclose(file);
    LOGI("Completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("Checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("Unable process volume! Skipping...\n");
        return 0;
    }
    
    ui_print("%s may be rfs. Checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("Checking for ext4 partitions...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("Done!\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%s", backup_name);

    ui_set_show_text(1);
    ui_print("Filesystems need to be converted to ext4.\n");
    ui_print("A backup and restore will now take place.\n");
    ui_print("If anything goes wrong, your backup will be\n");
    ui_print("named %s. Try restoring it\n", backup_name);
    ui_print("in case of error.\n");

    nandroid_backup(backup_path);
    nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("/tmp/recovery.log was copied to /sdcard/clockworkmod/recovery.log. Please open ROM Manager to report the issue.\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}

void backup_rom()
{
    static char* headers[] = {  "Select Your Current ROM",
                                "",
                                NULL
    };

     static char* list[] = {"~~~> Go Back <~~~",
                     "CyanogenMod 9",
			"CyanogenMod 7",
			"CyanogenMod 6.2",			
			"G3MOD",
			"Kyrillos",
			"Grigora",
			"Stylooo AOSP Style",
			"DutchMods",
			"Kyorarom",
			"Indroid",
			"Misc Rom1",
			"Misc Rom2",
			"Misc Rom3",
			"Misc Rom4",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	 if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
	    case 0:
	    {
		return;
		break;
	    }

	    case 1:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/CyanogenMod9_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 2:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/CyanogenMod7_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
            case 3:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/CyanogenMod6_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 4:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/G3MOD_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 5:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/Kyrillos_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }	
            case 6:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/Grigora_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	    case 7:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/AOSP_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	    case 8:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/DutchMods_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 9:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/Kyorarom_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 10:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/Indroid_ROM");
            nandroid_backup_system(backup_path);
    	    break;
       	    }
	    case 11:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/rom1_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	    case 12:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/rom2_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	    case 13:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/rom3_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	    case 14:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/roms/rom4_ROM");
            nandroid_backup_system(backup_path);
            break;
    	    }
	}
}
}
void backup_data()
{
    static char* headers[] = {  "Select Your Current ROM",
                                "",
                                NULL
    };

     static char* list[] = {"~~~> Go Back <~~~",
			"Samsung Based Froyo ROM",
			"CyanogenMod 6.2",			
			"CyanogenMod 7",
                     "CyanogenMod 9",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	 if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
	    case 0:
	    {
		return;
		break;
	    }
            case 1:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/data/Froyo_DATA");
            nandroid_backup_data(backup_path);
	    nandroid_backup_sd(backup_path);
	    nandroid_backup_androidSecure(backup_path);
	    return;
    	    break;
       	    }
	    case 2:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/data/CM6_DATA");
            nandroid_backup_data(backup_path);
	    nandroid_backup_sd(backup_path);
	    nandroid_backup_androidSecure(backup_path);
	    return;
    	    break;
       	    }
	    case 3:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/data/CM7_DATA");
            nandroid_backup_data(backup_path);
	    nandroid_backup_sd(backup_path);
	    nandroid_backup_androidSecure(backup_path);
	    return;
            break;
    	    }	
           case 4:
	    {
            char backup_path[PATH_MAX];
            sprintf(backup_path, "/sdcard/Android/data/g3mod/data/CM9_DATA");
            nandroid_backup_data(backup_path);
	    nandroid_backup_sd(backup_path);
	    nandroid_backup_androidSecure(backup_path);
	    return;
            break;
    	    }	
	}
}
}

void powermenu()
{

static char* headers[] = {  "Power Menu",
                                "",
                                NULL
    };

 static char* list[] = { "~~~> Go Back <~~~",
                         "Power Off",
                         "Reboot",
                         "Reboot Recovery",
                         "Download Mode",
                            NULL
};

for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	if (chosen_item == GO_BACK)
            break;
	switch (chosen_item)
	{
		case 0:
		{
		return;
		break;
		}
		case 1:
		{
		 __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL);
		break;
		}
		case 2:
		{
		reboot(RB_AUTOBOOT);
		break;
		}
		case 3:
		{
		__reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "recovery");
		break;
		}
		case 4:
		{
		__system("reboot download");
		break;
		}
	}
     }
}

static char**
prepend_title1(char** headers) {
    char* title[] = { EXPAND(RECOVERY_VERSION),
                      "",
                      NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

static void
wipe_data1(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "Confirm wipe of all user data?",
                                "  THIS CAN NOT BE UNDONE.",
                                "",
                                NULL };
            title_headers = prepend_title1(headers);
        }

        char* items[] = { " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " Yes -- delete all user data",   // [7]
                          " No",
                          " No",
                          " No",
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1, 0);
        if (chosen_item != 7) {
            return;
        }
    }

    ui_print("\n-- Wiping data...\n");
    device_wipe_data();
    erase_root1("/data");
#ifdef BOARD_HAS_DATADATA
    erase_root1("/datadata");
#endif
    erase_root1("/cache");
    erase_root1("/sd-ext");
    erase_root1("/sdcard/.android_secure");
    ui_print("Data wipe complete.\n");
}

static int
erase_root1(const char *root) {
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s...\n", root);
    return format_volume(root);
}

void show_wipe_menu()
{

static char* headers[] = {  "Wipe Menu",
                                "",
                                NULL
    };

 static char* list[] = { "~~~> Go Back <~~~",
                         "Data / Factory Reset",
                         "Cache",
                         "Wipe Dalvik Cache",
                         "Wipe Battery Stats",
                            NULL
};

for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	if (chosen_item == GO_BACK)
            break;
	switch (chosen_item)
	{
		case 0:
		{
		return;
		break;
		}
		case 1:
		{
		wipe_data1(ui_text_visible());
                if (!ui_text_visible()) return;
		break;
		}
		case 2:
		{
		  if (confirm_selection("Confirm wipe?", "Yes - Wipe Cache"))
                {
                    ui_print("\n-- Wiping cache...\n");
                    erase_root1("/cache");
                    ui_print("Cache wipe complete.\n");
                    if (!ui_text_visible()) return;
                }
		break;
		}
		case 3:
		{
		if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                }
                ensure_path_unmounted("/data");
                ui_print("Dalvik Cache wiped.\n");
                break;
		}
		case 4:
		{
		if (confirm_selection( "Confirm wipe?", "Yes - Wipe Battery Stats"))
                    wipe_battery_stats();
                break;
		}
	}
     }
}

void updatemenu()
{

static char* headers[] = {  "Update Menu",
                                "",
                                NULL
    };

 static char* list[] = { "~~~> Go Back <~~~",
			 "Update.zip from Sdcard",
                         "Select .zip from Sdcard",
                         "Toggle Signature Verification",
                         "Toggle Script Asserts",
                            NULL
};

for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	if (chosen_item == GO_BACK)
            break;
	switch (chosen_item)
	{
		case 0:
		{
		return;
		break;
		}
		case 1:
		 {
                if (confirm_selection("Confirm install?", "Yes - Install /sdcard/update.zip"))
                    install_zip(SDCARD_UPDATE_FILE);
                break;
            }
		case 2:
		{
		show_choose_zip_menu("/sdcard/");
                break;
		}
		case 3:
		 toggle_signature_check();
                break;
		case 4:
		toggle_script_asserts();
		break;
	}
     }
}

/* Modified by moikop */

void show_choose_kernel_menu() //show_choose_zip_menu() modification
{
    if (ensure_path_mounted("/sdcard") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose a zip to apply",
                                "",
                                NULL
    };

    char* file = choose_file_menu("/sdcard/Android/data/g3mod/kernel/", ".zip", headers);
    if (file == NULL)
        return;
    char sdcard_package_file[1024];
    strcpy(sdcard_package_file, "/sdcard/Android/data/g3mod/kernel/");
    strcat(sdcard_package_file,  file );
    static char* confirm_install  = "Confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(sdcard_package_file);
}

void create_rom_dirs() {

	// Rom Backup Dir
	__system("mkdir /sdcard/Android/data/g3mod/roms/CyanogenMod6_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/CyanogenMod7_ROM");
       __system("mkdir /sdcard/Android/data/g3mod/roms/CyanogenMod9_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/G3MOD_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/Kyrillos_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/Grigora_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/AOSP_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/DutchMods_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/Kyorarom_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/Indroid_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/rom1_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/rom2_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/rom3_ROM");
	__system("mkdir /sdcard/Android/data/g3mod/roms/rom4_ROM");
	
	//Data + sd-ext Backup Dir
	__system("mkdir /sdcard/Android/data/g3mod/data/Froyo_DATA");
	__system("mkdir /sdcard/Android/data/g3mod/data/CM6_DATA");
	__system("mkdir /sdcard/Android/data/g3mod/data/CM7_DATA");
       __system("mkdir /sdcard/Android/data/g3mod/data/CM9_DATA");
	
	//Kernel dir
	__system("mkdir /sdcard/Android/data/g3mod/kernel/Froyo");
	__system("mkdir /sdcard/Android/data/g3mod/kernel/CM6");
	__system("mkdir /sdcard/Android/data/g3mod/kernel/CM7");
       __system("mkdir /sdcard/Android/data/g3mod/kernel/CM9");
}

void show_multi_boot_menu()
{
    static char* headers[] = {  "MultiBoot Menu",
                                "",
                                NULL
    };

    static char* list[] = { "~~~> Go Back <~~~",
			    "Switch ROM",
                            "Backup Current ROM",
			    "Switch Kernel",
			    "Backup Data",
			    "Restore Data",
                            NULL
    };

    for (;;)
    {	
	create_rom_dirs();
        int chosen_item = get_menu_selection(headers, list, 0, 0);
	if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
		case 0:
		{
		return;
		break;
		}
            
		case 1:
                {
		if (ensure_path_mounted("/sdcard") != 0) {
		LOGE ("Can't mount /sdcard\n");
		return;
		}
		
   		static char* advancedheaders1[] = {  "Choose Which ROM to Activate",
                                			NULL
   						 };
  		char* file = choose_file_menu("/sdcard/Android/data/g3mod/roms/", NULL, advancedheaders1);
   		 if (file == NULL)
     		   return;

    		static char* headers[] = {  "Activating ROM",
                           		     "",
                            		    NULL
  					  };

    		static char* confirm_restore  = "Confirm activate?";
       	        if (confirm_selection(confirm_restore, "Yes - Activate ROM"))
		{		
		nandroid_restore_system(file,1);
		
		}
		
		if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                ensure_path_unmounted("/data");
                ui_print("Dalvik Cache wiped.\n");

		show_choose_kernel_menu();
		
                break;
	}
            case 2:
            {
                backup_rom();
                break;
            }
	case 3:
                {
		show_choose_kernel_menu();
                break;
	}
	case 4:
                {
		backup_data();
                break;
	}
	case 5:
                {
		if (ensure_path_mounted("/sdcard") != 0) {
		LOGE ("Can't mount /sdcard\n");
		return;
		}
		
   		static char* advancedheaders1[] = {  "Choose Which Data to Restore",
                                			NULL
   						 };
  		char* file = choose_file_menu("/sdcard/Android/data/g3mod/data/", NULL, advancedheaders1);
   		 if (file == NULL)
     		   return;

    		static char* headers[] = {  "Restoring Data",
                           		     "",
                            		    NULL
  					  };

    		static char* confirm_restore  = "Confirm restore?";
       	        if (confirm_selection(confirm_restore, "Yes - Restore Data"))
		{		
			nandroid_restore_data(file,1);
			nandroid_restore_sd(file,1);
			nandroid_restore_androidSecure(file,1);
		}
                break;
	}
        }
    }
}
