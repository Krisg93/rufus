/*
 * Rufus: The Reliable USB Formatting Utility
 * Copyright © 2011-2015 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <winioctl.h>
#include <shlobj.h>
#include <process.h>
#include <dbt.h>
#include <io.h>
#include <getopt.h>

#include "msapi_utf8.h"
#include "resource.h"
#include "rufus.h"
#include "drive.h"
#include "settings.h"
#include "localization.h"
#include "bled/bled.h"
#include "../res/grub/grub_version.h"
#include "../res/grub2/grub2_version.h"

/* Redefinitions for WDK and MinGW */
// TODO: these would be better in a 'missing.h' file
#ifndef PBM_SETSTATE
#define PBM_SETSTATE (WM_USER+16)
#endif
#ifndef PBST_NORMAL
#define PBST_NORMAL 1
#endif
#ifndef PBST_ERROR
#define PBST_ERROR 2
#endif
#ifndef PBST_PAUSED
#define PBST_PAUSED 3
#endif
#ifndef BUTTON_IMAGELIST_ALIGN_CENTER
#define BUTTON_IMAGELIST_ALIGN_CENTER 4
#endif
#ifndef BCM_SETIMAGELIST
#define BCM_SETIMAGELIST 0x1602
#endif
#ifndef DBT_CUSTOMEVENT
#define DBT_CUSTOMEVENT 0x8006
#endif

struct {
	HIMAGELIST himl;
	RECT margin;
	UINT uAlign;
} bi_iso = {0}, bi_up = {0}, bi_down = {0};	// BUTTON_IMAGELIST

typedef struct
{
	LPCITEMIDLIST pidl;
	BOOL fRecursive;
} MY_SHChangeNotifyEntry;

// MinGW doesn't know these
PF_TYPE(WINAPI, HIMAGELIST, ImageList_Create, (int, int, UINT, int, int));
PF_TYPE(WINAPI, int, ImageList_AddIcon, (HIMAGELIST, HICON));
PF_TYPE(WINAPI, int, ImageList_ReplaceIcon, (HIMAGELIST, int, HICON));
// WDK blows up when trying to using PF_TYPE_DECL() for the ImageList calls... so we don't.
PF_DECL(ImageList_Create);
PF_DECL(ImageList_AddIcon);
PF_DECL(ImageList_ReplaceIcon);
PF_TYPE_DECL(WINAPI, BOOL, SHChangeNotifyDeregister, (ULONG));
PF_TYPE_DECL(WINAPI, ULONG, SHChangeNotifyRegister, (HWND, int, LONG, UINT, int, const MY_SHChangeNotifyEntry*));

const char* cmdline_hogger = "rufus.com";
const char* FileSystemLabel[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS" };
// Number of steps for each FS for FCC_STRUCTURE_PROGRESS
const int nb_steps[FS_MAX] = { 5, 5, 12, 1, 10 };
static const char* PartitionTypeLabel[2] = { "MBR", "GPT" };
static BOOL existing_key = FALSE;	// For LGP set/restore
static BOOL size_check = TRUE;
static BOOL log_displayed = FALSE;
static BOOL iso_provided = FALSE;
static BOOL user_notified = FALSE;
static BOOL relaunch = FALSE;
extern BOOL force_large_fat32, enable_iso, enable_joliet, enable_rockridge, enable_ntfs_compression;
extern uint8_t* grub2_buf;
extern long grub2_len;
extern const char* old_c32_name[NB_OLD_C32];
static int selection_default;
static loc_cmd* selected_locale = NULL;
static UINT_PTR UM_LANGUAGE_MENU_MAX = UM_LANGUAGE_MENU;
static RECT relaunch_rc = { -65536, -65536, 0, 0};
static UINT uBootChecked = BST_CHECKED, uQFChecked = BST_CHECKED, uMBRChecked = BST_UNCHECKED;
static HFONT hFont;
static WNDPROC info_original_proc = NULL;
char ClusterSizeLabel[MAX_CLUSTER_SIZES][64];
char msgbox[1024], msgbox_title[32], *ini_file = NULL;
char lost_translators[][6] = LOST_TRANSLATORS;

/*
 * Globals
 */
OPENED_LIBRARIES_VARS;
HINSTANCE hMainInstance;
HWND hMainDialog, hLangToolbar = NULL;
char szFolderPath[MAX_PATH], app_dir[MAX_PATH];
char* image_path = NULL;
float fScale = 1.0f;
int default_fs;
uint32_t dur_mins, dur_secs;
HWND hDeviceList, hPartitionScheme, hFileSystem, hClusterSize, hLabel, hBootType, hNBPasses, hLog = NULL;
HWND hLogDlg = NULL, hProgress = NULL, hInfo, hDiskID;
BOOL use_own_c32[NB_OLD_C32] = {FALSE, FALSE}, detect_fakes = TRUE, mbr_selected_by_user = FALSE;
BOOL iso_op_in_progress = FALSE, format_op_in_progress = FALSE, right_to_left_mode = FALSE;
BOOL enable_HDDs = FALSE, advanced_mode = TRUE, force_update = FALSE, use_fake_units = TRUE;
BOOL allow_dual_uefi_bios = FALSE, enable_vmdk = FALSE, togo_mode = TRUE;
int dialog_showing = 0, lang_button_id = 0;
uint16_t rufus_version[3], embedded_sl_version[2];
char embedded_sl_version_str[2][12] = { "?.??", "?.??" };
char embedded_sl_version_ext[2][32];
RUFUS_UPDATE update = { {0,0,0}, {0,0}, NULL, NULL};
StrArray DriveID, DriveLabel;
extern char* szStatusMessage;

static HANDLE format_thid = NULL;
static HWND hBoot = NULL, hSelectISO = NULL;
static HICON hIconDisc, hIconDown, hIconUp, hIconLang;
static char szTimer[12] = "00:00:00";
static unsigned int timer;
static int64_t last_iso_blocking_status;
static void ToggleToGo(void);

/*
 * The following is used to allocate slots within the progress bar
 * 0 means unused (no operation or no progress allocated to it)
 * +n means allocate exactly n bars (n percent of the progress bar)
 * -n means allocate a weighted slot of n from all remaining
 *    bars. E.g. if 80 slots remain and the sum of all negative entries
 *    is 10, -4 will allocate 4/10*80 = 32 bars (32%) for OP progress
 */
static int nb_slots[OP_MAX];
static float slot_end[OP_MAX+1];	// shifted +1 so that we can subtract 1 to OP indexes
static float previous_end;

// TODO: Remember to update copyright year in stdlg's AboutCallback() WM_INITDIALOG,
// localization_data.sh and the .rc when the year changes!

#define KB          1024LL
#define MB       1048576LL
#define GB    1073741824LL
#define TB 1099511627776LL

/*
 * Fill in the cluster size names
 */
static void SetClusterSizeLabels(void)
{
	unsigned int i, j, k;
	safe_sprintf(ClusterSizeLabel[0], 64, lmprintf(MSG_029));
	for (i=512, j=1, k=MSG_026; j<MAX_CLUSTER_SIZES; i<<=1, j++) {
		if (i > 8192) {
			i /= 1024;
			k++;
		}
		safe_sprintf(ClusterSizeLabel[j], 64, "%d %s", i, lmprintf(k));
	}
}

/*
 * Set cluster size values according to http://support.microsoft.com/kb/140365
 * this call will return FALSE if we can't find a supportable FS for the drive
 */
static BOOL DefineClusterSizes(void)
{
	LONGLONG i;
	int fs;
	BOOL r = FALSE;
	char tmp[128] = "", *entry;

	default_fs = FS_UNKNOWN;
	memset(&SelectedDrive.ClusterSize, 0, sizeof(SelectedDrive.ClusterSize));
	if (SelectedDrive.DiskSize < 8*MB) {
		uprintf("Device was eliminated because it is smaller than 8 MB\n");
		goto out;
	}

/*
 * The following are MS's allowed cluster sizes for FAT16 and FAT32:
 *
 * FAT16
 * 31M  :  512 - 4096
 * 63M  : 1024 - 8192
 * 127M : 2048 - 16k
 * 255M : 4096 - 32k
 * 511M : 8192 - 64k
 * 1023M:  16k - 64k
 * 2047M:  32k - 64k
 * 4095M:  64k
 * 4GB+ : N/A
 *
 * FAT32
 * 31M  : N/A
 * 63M  : N/A			(NB unlike MS, we're allowing 512-512 here)
 * 127M :  512 - 1024
 * 255M :  512 - 2048
 * 511M :  512 - 4096
 * 1023M:  512 - 8192
 * 2047M:  512 - 16k
 * 4095M: 1024 - 32k
 * 7GB  : 2048 - 64k
 * 15GB : 4096 - 64k
 * 31GB : 8192 - 64k This is as far as Microsoft's FormatEx goes...
 * 63GB :  16k - 64k ...but we can go higher using fat32format from RidgeCrop.
 * 2TB+ : N/A
 *
 */

	// FAT 16
	if (SelectedDrive.DiskSize < 4*GB) {
		SelectedDrive.ClusterSize[FS_FAT16].Allowed = 0x00001E00;
		for (i=32; i<=4096; i<<=1) {			// 8 MB -> 4 GB
			if (SelectedDrive.DiskSize < i*MB) {
				SelectedDrive.ClusterSize[FS_FAT16].Default = 16*(ULONG)i;
				break;
			}
			SelectedDrive.ClusterSize[FS_FAT16].Allowed <<= 1;
		}
		SelectedDrive.ClusterSize[FS_FAT16].Allowed &= 0x0001FE00;
	}

	// FAT 32
	// > 32GB FAT32 is not supported by MS and FormatEx but is achieved using fat32format
	// See: http://www.ridgecrop.demon.co.uk/index.htm?fat32format.htm
	// < 32 MB FAT32 is not allowed by FormatEx, so we don't bother
	if ((SelectedDrive.DiskSize >= 32*MB) && (1.0f*SelectedDrive.DiskSize < 1.0f*MAX_FAT32_SIZE*TB)) {
		SelectedDrive.ClusterSize[FS_FAT32].Allowed = 0x000001F8;
		for (i=32; i<=(32*1024); i<<=1) {			// 32 MB -> 32 GB
			if (SelectedDrive.DiskSize*1.0f < i*MB*FAT32_CLUSTER_THRESHOLD) {	// MS
				SelectedDrive.ClusterSize[FS_FAT32].Default = 8*(ULONG)i;
				break;
			}
			SelectedDrive.ClusterSize[FS_FAT32].Allowed <<= 1;
		}
		SelectedDrive.ClusterSize[FS_FAT32].Allowed &= 0x0001FE00;

		// Default cluster sizes in the 256MB to 32 GB range do not follow the rule above
		if ((SelectedDrive.DiskSize >= 256*MB) && (SelectedDrive.DiskSize < 32*GB)) {
			for (i=8; i<=32; i<<=1) {				// 256 MB -> 32 GB
				if (SelectedDrive.DiskSize*1.0f < i*GB*FAT32_CLUSTER_THRESHOLD) {
					SelectedDrive.ClusterSize[FS_FAT32].Default = ((ULONG)i/2)*1024;
					break;
				}
			}
		}
		// More adjustments for large drives
		if (SelectedDrive.DiskSize >= 32*GB) {
			SelectedDrive.ClusterSize[FS_FAT32].Allowed &= 0x0001C000;
			SelectedDrive.ClusterSize[FS_FAT32].Default = 0x00008000;
		}
	}

	if (SelectedDrive.DiskSize < 256*TB) {
		// NTFS
		SelectedDrive.ClusterSize[FS_NTFS].Allowed = 0x0001FE00;
		for (i=16; i<=256; i<<=1) {				// 7 MB -> 256 TB
			if (SelectedDrive.DiskSize < i*TB) {
				SelectedDrive.ClusterSize[FS_NTFS].Default = ((ULONG)i/4)*1024;
				break;
			}
		}

		// exFAT (requires KB955704 installed on XP => don't bother)
		if (nWindowsVersion > WINDOWS_XP) {
			SelectedDrive.ClusterSize[FS_EXFAT].Allowed = 0x03FFFE00;
			if (SelectedDrive.DiskSize < 256*MB)	// < 256 MB
				SelectedDrive.ClusterSize[FS_EXFAT].Default = 4*1024;
			else if (SelectedDrive.DiskSize < 32*GB)	// < 32 GB
				SelectedDrive.ClusterSize[FS_EXFAT].Default = 32*1024;
			else
				SelectedDrive.ClusterSize[FS_EXFAT].Default = 28*1024;
		}

		// UDF (only supported for Vista and later)
		if (nWindowsVersion >= WINDOWS_VISTA) {
			SelectedDrive.ClusterSize[FS_UDF].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
			SelectedDrive.ClusterSize[FS_UDF].Default = 1;
		}

		// ReFS (only supported for Windows 8.1 and later and for fixed disks)
		if (SelectedDrive.DiskSize >= 512*MB) {
			if ((nWindowsVersion >= WINDOWS_8_1) && (SelectedDrive.Geometry.MediaType == FixedMedia)) {
				SelectedDrive.ClusterSize[FS_REFS].Allowed = SINGLE_CLUSTERSIZE_DEFAULT;
				SelectedDrive.ClusterSize[FS_REFS].Default = 1;
			}
		}
	}

out:
	// Only add the filesystems we can service
	for (fs=0; fs<FS_MAX; fs++) {
		// Remove all cluster sizes that are below the sector size
		if (SelectedDrive.ClusterSize[fs].Allowed != SINGLE_CLUSTERSIZE_DEFAULT) {
			SelectedDrive.ClusterSize[fs].Allowed &= ~(SelectedDrive.Geometry.BytesPerSector - 1);
			if ((SelectedDrive.ClusterSize[fs].Default & SelectedDrive.ClusterSize[fs].Allowed) == 0)
				// We lost our default => Use rightmost bit to select the new one
				SelectedDrive.ClusterSize[fs].Default =
					SelectedDrive.ClusterSize[fs].Allowed & (-(LONG)SelectedDrive.ClusterSize[fs].Allowed);
		}

		if (SelectedDrive.ClusterSize[fs].Allowed != 0) {
			tmp[0] = 0;
			// Tell the user if we're going to use Large FAT32 or regular
			if ((fs == FS_FAT32) && ((SelectedDrive.DiskSize > LARGE_FAT32_SIZE) || (force_large_fat32)))
				safe_strcat(tmp, sizeof(tmp), "Large ");
			safe_strcat(tmp, sizeof(tmp), FileSystemLabel[fs]);
			if (default_fs == FS_UNKNOWN) {
				entry = lmprintf(MSG_030, tmp);
				default_fs = fs;
			} else {
				entry = tmp;
			}
			IGNORE_RETVAL(ComboBox_SetItemData(hFileSystem,
				ComboBox_AddStringU(hFileSystem, entry), fs));
			r = TRUE;
		}
	}

	return r;
}
#undef KB
#undef MB
#undef GB
#undef TB

/*
 * Populate the Allocation unit size field
 */
static BOOL SetClusterSizes(int FSType)
{
	char* szClustSize;
	int i, k, default_index = 0;
	ULONG j;

	IGNORE_RETVAL(ComboBox_ResetContent(hClusterSize));

	if ((FSType < 0) || (FSType >= FS_MAX)) {
		return FALSE;
	}

	if ( (SelectedDrive.ClusterSize[FSType].Allowed == 0)
	  || (SelectedDrive.ClusterSize[FSType].Default == 0) ) {
		uprintf("The drive is incompatible with FS type #%d\n", FSType);
		return FALSE;
	}

	for(i=0,j=0x100,k=0;j<0x10000000;i++,j<<=1) {
		if (j & SelectedDrive.ClusterSize[FSType].Allowed) {
			if (j == SelectedDrive.ClusterSize[FSType].Default) {
				szClustSize = lmprintf(MSG_030, ClusterSizeLabel[i]);
				default_index = k;
			} else {
				szClustSize = ClusterSizeLabel[i];
			}
			IGNORE_RETVAL(ComboBox_SetItemData(hClusterSize, ComboBox_AddStringU(hClusterSize, szClustSize), j));
			k++;
		}
	}

	IGNORE_RETVAL(ComboBox_SetCurSel(hClusterSize, default_index));
	return TRUE;
}

/*
 * Fill the drive properties (size, FS, etc)
 */
static BOOL SetDriveInfo(int ComboIndex)
{
	DWORD i;
	int pt, bt;
	char fs_type[32];

	memset(&SelectedDrive, 0, sizeof(SelectedDrive));
	SelectedDrive.DeviceNumber = (DWORD)ComboBox_GetItemData(hDeviceList, ComboIndex);

	GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_type, sizeof(fs_type), FALSE);

	if (!DefineClusterSizes()) {
		uprintf("No file system is selectable for this drive\n");
		return FALSE;
	}

	// re-select existing FS if it's one we know
	SelectedDrive.FSType = FS_UNKNOWN;
	if (safe_strlen(fs_type) != 0) {
		for (SelectedDrive.FSType=FS_MAX-1; SelectedDrive.FSType>=0; SelectedDrive.FSType--) {
			if (safe_strcmp(fs_type, FileSystemLabel[SelectedDrive.FSType]) == 0) {
				break;
			}
		}
	} else {
		SelectedDrive.FSType = FS_UNKNOWN;
	}

	for (i=0; i<ComboBox_GetCount(hFileSystem); i++) {
		if (ComboBox_GetItemData(hFileSystem, i) == SelectedDrive.FSType) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
			break;
		}
	}

	if (i == ComboBox_GetCount(hFileSystem)) {
		// failed to reselect => pick default
		for (i=0; i<ComboBox_GetCount(hFileSystem); i++) {
			if (ComboBox_GetItemData(hFileSystem, i) == default_fs) {
				IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
				break;
			}
		}
	}

	for (i=0; i<3; i++) {
		// Populate MBR/BIOS, MBR/UEFI and GPT/UEFI targets, with an exception
		// for XP, as it doesn't support GPT at all
		if ((i == 2) && (nWindowsVersion <= WINDOWS_XP))
			continue;
		bt = (i==0)?BT_BIOS:BT_UEFI;
		pt = (i==2)?PARTITION_STYLE_GPT:PARTITION_STYLE_MBR;
		IGNORE_RETVAL(ComboBox_SetItemData(hPartitionScheme, ComboBox_AddStringU(hPartitionScheme,
			lmprintf((i==0)?MSG_031:MSG_033, PartitionTypeLabel[pt])), (bt<<16)|pt));
	}

	// At least one filesystem is go => enable formatting
	EnableWindow(GetDlgItem(hMainDialog, IDC_START), TRUE);

	return SetClusterSizes((int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem)));
}

static void SetFSFromISO(void)
{
	int i, fs, selected_fs = FS_UNKNOWN;
	uint32_t fs_mask = 0;
	int bt = GETBIOSTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
	BOOL windows_to_go = (togo_mode) && HAS_TOGO(iso_report) &&
		(Button_GetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO)) == BST_CHECKED);

	if (image_path == NULL)
		return;

	// Create a mask of all the FS's available
	for (i=0; i<ComboBox_GetCount(hFileSystem); i++) {
		fs = (int)ComboBox_GetItemData(hFileSystem, i);
		fs_mask |= 1<<fs;
	}

	// Syslinux and EFI have precedence over bootmgr (unless the user selected BIOS as target type)
	if ((HAS_SYSLINUX(iso_report)) || (IS_REACTOS(iso_report)) || (iso_report.has_kolibrios) ||
		((iso_report.has_efi) && (bt == BT_UEFI) && (!iso_report.has_4GB_file) && (!windows_to_go))) {
		if (fs_mask & (1<<FS_FAT32)) {
			selected_fs = FS_FAT32;
		} else if ((fs_mask & (1<<FS_FAT16)) && (!iso_report.has_kolibrios)) {
			selected_fs = FS_FAT16;
		}
	} else if ((windows_to_go) || (iso_report.has_bootmgr) || (IS_WINPE(iso_report.winpe))) {
		if (fs_mask & (1<<FS_NTFS)) {
			selected_fs = FS_NTFS;
		}
	}

	// Try to select the FS
	for (i=0; i<ComboBox_GetCount(hFileSystem); i++) {
		fs = (int)ComboBox_GetItemData(hFileSystem, i);
		if (fs == selected_fs)
			IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
	}

	SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
		ComboBox_GetCurSel(hFileSystem));
}

static void SetMBRProps(void)
{
	int fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
	int dt = (int)ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
	BOOL needs_masquerading = (IS_WINPE(iso_report.winpe) && (!iso_report.uses_minint));

	if ((!mbr_selected_by_user) && ((image_path == NULL) || (dt != DT_ISO) || (fs != FS_NTFS) || IS_GRUB(iso_report) ||
		((togo_mode) && (Button_GetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO)) == BST_CHECKED)) )) {
		CheckDlgButton(hMainDialog, IDC_RUFUS_MBR, BST_UNCHECKED);
		IGNORE_RETVAL(ComboBox_SetCurSel(hDiskID, 0));
		return;
	}

	uMBRChecked = (needs_masquerading || iso_report.has_bootmgr || mbr_selected_by_user)?BST_CHECKED:BST_UNCHECKED;
	if (IsWindowEnabled(GetDlgItem(hMainDialog, IDC_RUFUS_MBR)))
		CheckDlgButton(hMainDialog, IDC_RUFUS_MBR, uMBRChecked);
	IGNORE_RETVAL(ComboBox_SetCurSel(hDiskID, needs_masquerading?1:0));
}

static void SetToGo(void)
{
	int dt = (int)ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
	if ( ((dt != DT_ISO) && (togo_mode)) || ((dt == DT_ISO) && (HAS_TOGO(iso_report)) && (!togo_mode)) )
		ToggleToGo();
}

static void EnableAdvancedBootOptions(BOOL enable, BOOL remove_checkboxes)
{
	int bt = GETBIOSTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
	BOOL actual_enable_mbr = ((bt==BT_UEFI)||(selection_default>=DT_IMG)||!IsChecked(IDC_BOOT))?FALSE:enable;
	BOOL actual_enable_fix = ((bt==BT_UEFI)||(selection_default==DT_IMG)||!IsChecked(IDC_BOOT))?FALSE:enable;
	static UINT uXPartChecked = BST_UNCHECKED;

	if ((selection_default == DT_ISO) && (iso_report.has_kolibrios || IS_GRUB(iso_report) || IS_REACTOS(iso_report) || HAS_SYSLINUX(iso_report))) {
		actual_enable_mbr = FALSE;
		mbr_selected_by_user = FALSE;
	}
	if (remove_checkboxes) {
		// Store/Restore the checkbox states
		if (IsWindowEnabled(GetDlgItem(hMainDialog, IDC_RUFUS_MBR)) && !actual_enable_mbr) {
			uMBRChecked = IsChecked(IDC_RUFUS_MBR);
			CheckDlgButton(hMainDialog, IDC_RUFUS_MBR, BST_UNCHECKED);
			uXPartChecked = IsChecked(IDC_EXTRA_PARTITION);
			CheckDlgButton(hMainDialog, IDC_EXTRA_PARTITION, BST_UNCHECKED);
		} else if (!IsWindowEnabled(GetDlgItem(hMainDialog, IDC_RUFUS_MBR)) && actual_enable_mbr) {
			CheckDlgButton(hMainDialog, IDC_RUFUS_MBR, uMBRChecked);
			CheckDlgButton(hMainDialog, IDC_EXTRA_PARTITION, uXPartChecked);
		}
	}

	EnableWindow(GetDlgItem(hMainDialog, IDC_EXTRA_PARTITION), actual_enable_fix);
	EnableWindow(GetDlgItem(hMainDialog, IDC_RUFUS_MBR), actual_enable_mbr);
	EnableWindow(hDiskID, actual_enable_mbr);
}

static void EnableBootOptions(BOOL enable, BOOL remove_checkboxes)
{
	int fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
	BOOL actual_enable = ((!IS_FAT(fs)) && (fs != FS_NTFS) && (selection_default == DT_IMG))?FALSE:enable;

	EnableWindow(hBoot, actual_enable);
	EnableWindow(hBootType, actual_enable);
	EnableWindow(hSelectISO, actual_enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL), actual_enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO), actual_enable);
	EnableAdvancedBootOptions(actual_enable, remove_checkboxes);
}

static void SetPartitionSchemeTooltip(void)
{
	int bt = GETBIOSTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
	int pt = GETPARTTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
	if (bt == BT_BIOS) {
		CreateTooltip(hPartitionScheme, lmprintf(MSG_150), 15000);
	} else {
		if (pt == PARTITION_STYLE_MBR) {
			CreateTooltip(hPartitionScheme, lmprintf(MSG_151), 15000);
		} else {
			CreateTooltip(hPartitionScheme, lmprintf(MSG_152), 15000);
		}
	}
}

static void SetTargetSystem(void)
{
	int ts;

	if (SelectedDrive.PartitionType == PARTITION_STYLE_GPT) {
		ts = 2;	// GPT/UEFI
	} else if (SelectedDrive.has_protective_mbr || SelectedDrive.has_mbr_uefi_marker || ((iso_report.has_efi) &&
		(!HAS_SYSLINUX(iso_report)) && (!iso_report.has_bootmgr) && (!IS_REACTOS(iso_report)) && 
		(!iso_report.has_kolibrios) && (!IS_GRUB(iso_report)) && (!IS_WINPE(iso_report.winpe))) ) {
		ts = 1;	// MBR/UEFI
	} else {
		ts = 0;	// MBR/BIOS|UEFI
	}
	IGNORE_RETVAL(ComboBox_SetCurSel(hPartitionScheme, ts));
	SetPartitionSchemeTooltip();
}

/*
 * Populate the UI properties
 */
static BOOL PopulateProperties(int ComboIndex)
{
	const char no_label[] = STR_NO_LABEL;
	char* device_tooltip;

	IGNORE_RETVAL(ComboBox_ResetContent(hPartitionScheme));
	IGNORE_RETVAL(ComboBox_ResetContent(hFileSystem));
	IGNORE_RETVAL(ComboBox_ResetContent(hClusterSize));
	EnableWindow(GetDlgItem(hMainDialog, IDC_START), FALSE);
	SetWindowTextA(hLabel, "");
	memset(&SelectedDrive, 0, sizeof(SelectedDrive));

	if (ComboIndex < 0)
		return TRUE;

	if (!SetDriveInfo(ComboIndex))	// This also populates FS
		return FALSE;
	SetTargetSystem();
	SetFSFromISO();
	EnableBootOptions(TRUE, TRUE);

	// Set a proposed label according to the size (eg: "256MB", "8GB")
	safe_sprintf(SelectedDrive.proposed_label, sizeof(SelectedDrive.proposed_label),
		SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, use_fake_units));

	// Add a tooltip (with the size of the device in parenthesis)
	device_tooltip = (char*) malloc(safe_strlen(DriveID.String[ComboIndex]) + 16);
	if (device_tooltip != NULL) {
		safe_sprintf(device_tooltip, safe_strlen(DriveID.String[ComboIndex]) + 16, "%s (%s)",
			DriveID.String[ComboIndex], SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, FALSE));
		CreateTooltip(hDeviceList, device_tooltip, -1);
		free(device_tooltip);
	}

	// If no existing label is available and no ISO is selected, propose one according to the size (eg: "256MB", "8GB")
	if ((image_path == NULL) || (iso_report.label[0] == 0)) {
		if ( (safe_stricmp(no_label, DriveLabel.String[ComboIndex]) == 0)
		  || (safe_stricmp(lmprintf(MSG_207), DriveLabel.String[ComboIndex]) == 0) ) {
			SetWindowTextU(hLabel, SelectedDrive.proposed_label);
		} else {
			SetWindowTextU(hLabel, DriveLabel.String[ComboIndex]);
		}
	} else {
		SetWindowTextU(hLabel, iso_report.label);
	}

	return TRUE;
}

/*
 * Set up progress bar real estate allocation
 */
static void InitProgress(BOOL bOnlyFormat)
{
	int i, fs;
	float last_end = 0.0f, slots_discrete = 0.0f, slots_analog = 0.0f;

	fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));

	memset(nb_slots, 0, sizeof(nb_slots));
	memset(slot_end, 0, sizeof(slot_end));
	previous_end = 0.0f;

	if (bOnlyFormat) {
		nb_slots[OP_FORMAT] = -1;
	} else {
		nb_slots[OP_ANALYZE_MBR] = 1;
		if (IsChecked(IDC_BADBLOCKS)) {
			nb_slots[OP_BADBLOCKS] = -1;
		}
		if (IsChecked(IDC_BOOT)) {
			// 1 extra slot for PBR writing
			switch (selection_default) {
			case DT_WINME:
				nb_slots[OP_DOS] = 3+1;
				break;
			case DT_FREEDOS:
				nb_slots[OP_DOS] = 5+1;
				break;
			case DT_IMG:
				nb_slots[OP_DOS] = 0;
				break;
			case DT_ISO:
				nb_slots[OP_DOS] = -1;
				break;
			default:
				nb_slots[OP_DOS] = 2+1;
				break;
			}
		}
		if (selection_default == DT_IMG) {
			nb_slots[OP_FORMAT] = -1;
		} else {
			nb_slots[OP_ZERO_MBR] = 1;
			nb_slots[OP_PARTITION] = 1;
			nb_slots[OP_FIX_MBR] = 1;
			nb_slots[OP_CREATE_FS] =
				nb_steps[ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem))];
			if ( (!IsChecked(IDC_QUICKFORMAT))
			  || ((fs == FS_FAT32) && ((SelectedDrive.DiskSize >= LARGE_FAT32_SIZE) || (force_large_fat32))) ) {
				nb_slots[OP_FORMAT] = -1;
			}
			nb_slots[OP_FINALIZE] = ((selection_default == DT_ISO) && (fs == FS_NTFS))?3:2;
		}
	}

	for (i=0; i<OP_MAX; i++) {
		if (nb_slots[i] > 0) {
			slots_discrete += nb_slots[i]*1.0f;
		}
		if (nb_slots[i] < 0) {
			slots_analog += nb_slots[i]*1.0f;
		}
	}

	for (i=0; i<OP_MAX; i++) {
		if (nb_slots[i] == 0) {
			slot_end[i+1] = last_end;
		} else if (nb_slots[i] > 0) {
			slot_end[i+1] = last_end + (1.0f * nb_slots[i]);
		} else if (nb_slots[i] < 0) {
			slot_end[i+1] = last_end + (( (100.0f-slots_discrete) * nb_slots[i]) / slots_analog);
		}
		last_end = slot_end[i+1];
	}

	/* Is there's no analog, adjust our discrete ends to fill the whole bar */
	if (slots_analog == 0.0f) {
		for (i=0; i<OP_MAX; i++) {
			slot_end[i+1] *= 100.0f / slots_discrete;
		}
	}
}

/*
 * Position the progress bar within each operation range
 */
void UpdateProgress(int op, float percent)
{
	int pos;

	if ((op < 0) || (op >= OP_MAX)) {
		duprintf("UpdateProgress: invalid op %d\n", op);
		return;
	}
	if (percent > 100.1f) {
//		duprintf("UpdateProgress(%d): invalid percentage %0.2f\n", op, percent);
		return;
	}
	if ((percent < 0.0f) && (nb_slots[op] <= 0)) {
		duprintf("UpdateProgress(%d): error negative percentage sent for negative slot value\n", op);
		return;
	}
	if (nb_slots[op] == 0)
		return;
	if (previous_end < slot_end[op]) {
		previous_end = slot_end[op];
	}

	if (percent < 0.0f) {
		// Negative means advance one slot (1.0%) - requires a positive slot allocation
		previous_end += (slot_end[op+1] - slot_end[op]) / (1.0f * nb_slots[op]);
		pos = (int)(previous_end / 100.0f * MAX_PROGRESS);
	} else {
		pos = (int)((previous_end + ((slot_end[op+1] - previous_end) * (percent / 100.0f))) / 100.0f * MAX_PROGRESS);
	}
	if (pos > MAX_PROGRESS) {
		duprintf("UpdateProgress(%d): rounding error - pos %d is greater than %d\n", op, pos, MAX_PROGRESS);
		pos = MAX_PROGRESS;
	}

	SendMessage(hProgress, PBM_SETPOS, (WPARAM)pos, 0);
	SetTaskbarProgressValue(pos, MAX_PROGRESS);
}

/*
 * Toggle controls according to operation
 */
static void EnableControls(BOOL bEnable)
{
	EnableWindow(GetDlgItem(hMainDialog, IDC_DEVICE), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_START), (ComboBox_GetCurSel(hDeviceList)<0)?FALSE:bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_ABOUT), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_BADBLOCKS), bEnable);
	EnableBootOptions(bEnable, FALSE);
	EnableWindow(hSelectISO, bEnable);
	EnableWindow(hNBPasses, bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_ADVANCED), bEnable);
	EnableWindow(hLangToolbar, bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_ENABLE_FIXED_DISKS), bEnable);
	SetDlgItemTextU(hMainDialog, IDCANCEL, lmprintf(bEnable?MSG_006:MSG_007));
	if (selection_default == DT_IMG)
		return;
	EnableWindow(GetDlgItem(hMainDialog, IDC_PARTITION_TYPE), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_FILESYSTEM), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_CLUSTERSIZE), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_LABEL), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_QUICKFORMAT), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_SET_ICON), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL), bEnable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO), bEnable);
}

/* Callback for the log window */
BOOL CALLBACK LogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	HFONT hf;
	long lfHeight;
	DWORD log_size;
	char *log_buffer = NULL, *filepath;
	EXT_DECL(log_ext, "rufus.log", __VA_GROUP__("*.log"), __VA_GROUP__("Rufus log"));
	switch (message) {
	case WM_INITDIALOG:
		apply_localization(IDD_LOG, hDlg);
		hLog = GetDlgItem(hDlg, IDC_LOG_EDIT);
		// Increase the size of our log textbox to MAX_LOG_SIZE (unsigned word)
		PostMessage(hLog, EM_LIMITTEXT, MAX_LOG_SIZE , 0);
		// Set the font to Unicode so that we can display anything
		hdc = GetDC(NULL);
		lfHeight = -MulDiv(8, GetDeviceCaps(hdc, LOGPIXELSY), 72);
		ReleaseDC(NULL, hdc);
		hf = CreateFontA(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, 0, 0, PROOF_QUALITY, 0, "Arial Unicode MS");
		SendDlgItemMessageA(hDlg, IDC_LOG_EDIT, WM_SETFONT, (WPARAM)hf, TRUE);
		// Set 'Close Log' as the selected button
		SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDCANCEL), TRUE);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDCANCEL:
			ShowWindow(hDlg, SW_HIDE);
			log_displayed = FALSE;
			return TRUE;
		case IDC_LOG_CLEAR:
			SetWindowTextA(hLog, "");
			return TRUE;
		case IDC_LOG_SAVE:
			log_size = GetWindowTextLengthU(hLog);
			if (log_size <= 0)
				break;
			log_buffer = (char*)malloc(log_size);
			if (log_buffer != NULL) {
				log_size = GetDlgItemTextU(hDlg, IDC_LOG_EDIT, log_buffer, log_size);
				if (log_size != 0) {
					log_size--;	// remove NUL terminator
					filepath =  FileDialog(TRUE, app_dir, &log_ext, 0);
					if (filepath != NULL) {
						FileIO(TRUE, filepath, &log_buffer, &log_size);
					}
					safe_free(filepath);
				}
				safe_free(log_buffer);
			}
			break;
		}
		break;
	case WM_CLOSE:
		ShowWindow(hDlg, SW_HIDE);
		reset_localization(IDD_LOG);
		log_displayed = FALSE;
		return TRUE;
	}
	return FALSE;
}

/*
 * Timer in the right part of the status area
 */
static void CALLBACK ClockTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	timer++;
	safe_sprintf(szTimer, sizeof(szTimer), "%02d:%02d:%02d",
		timer/3600, (timer%3600)/60, timer%60);
	SendMessageA(GetDlgItem(hWnd, IDC_STATUS), SB_SETTEXTA, SBT_OWNERDRAW | 1, (LPARAM)szTimer);
}

/*
 * Device Refresh Timer
 */
static void CALLBACK RefreshTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	// DO NOT USE WM_DEVICECHANGE - IT MAY BE FILTERED OUT BY WINDOWS!
	SendMessage(hWnd, UM_MEDIA_CHANGE, 0, 0);
}

/*
 * Detect and notify about a blocking operation during ISO extraction cancellation
 */
static void CALLBACK BlockingTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if (iso_blocking_status < 0) {
		KillTimer(hMainDialog, TID_BLOCKING_TIMER);
		user_notified = FALSE;
		uprintf("Killed blocking I/O timer\n");
	} else if(!user_notified) {
		if (last_iso_blocking_status == iso_blocking_status) {
			// A write or close operation hasn't made any progress since our last check
			user_notified = TRUE;
			uprintf("Blocking I/O operation detected\n");
			MessageBoxU(hMainDialog, lmprintf(MSG_080), lmprintf(MSG_048), MB_OK|MB_ICONINFORMATION|MB_IS_RTL);
		} else {
			last_iso_blocking_status = iso_blocking_status;
		}
	}
}

// Randomly nag users about translations that have been left behind
void LostTranslatorCheck(void)
{
	char *p;
	char* lang = safe_strdup(selected_locale->txt[1]);
	int i, r = rand() * LOST_TRANSLATOR_PROBABILITY / RAND_MAX;
	for (i=0; i<ARRAYSIZE(lost_translators); i++)
		if (strcmp(selected_locale->txt[0], lost_translators[i]) == 0)
			break;
	if ((r == 0) && (i != ARRAYSIZE(lost_translators)) && (lang != NULL) && ((p = strchr(lang, '(')) != NULL)) {
		p[-1] = 0;
		safe_sprintf(msgbox, sizeof(msgbox), "Note: The %s translation requires an update, but the original "
			"translator is no longer contributing to it...\nIf you can read English and want to help complete "
			"this translation, please visit: http://rufus.akeo.ie/translate.", lang);
		MessageBoxU(hMainDialog, msgbox, "Translation help needed", MB_OK|MB_ICONINFORMATION);
	}
	safe_free(lang);
}

// Report the features of the selected ISO images
static const char* YesNo(BOOL b) {
	return (b) ? "Yes" : "No";
}
static void DisplayISOProps(void)
{
	int i;
	char isolinux_str[16] = "No";

	if (HAS_SYSLINUX(iso_report)) {
		safe_sprintf(isolinux_str, sizeof(isolinux_str), "Yes (%s)", iso_report.sl_version_str);
	}

	// TODO: Only report features that are present
	uprintf("ISO label: %s", iso_report.label);
	uprintf("  Size: %" PRIu64 " bytes", iso_report.projected_size);
	uprintf("  Has a >64 chars filename: %s", YesNo(iso_report.has_long_filename));
	uprintf("  Has Symlinks: %s", YesNo(iso_report.has_symlinks));
	uprintf("  Has a >4GB file: %s", YesNo(iso_report.has_4GB_file));
	uprintf("  Uses Bootmgr: %s", YesNo(iso_report.has_bootmgr));
	uprintf("  Uses EFI: %s%s", YesNo(iso_report.has_efi), IS_WIN7_EFI(iso_report) ? " (win7_x64)" : "");
	uprintf("  Uses Grub 2: %s", YesNo(iso_report.has_grub2));
	uprintf("  Uses Grub4DOS: %s", YesNo(iso_report.has_grub4dos));
	uprintf("  Uses isolinux: %s", isolinux_str);
	if (HAS_SYSLINUX(iso_report) && (SL_MAJOR(iso_report.sl_version) < 5)) {
		for (i = 0; i<NB_OLD_C32; i++) {
			uprintf("    With an old %s: %s\n", old_c32_name[i], iso_report.has_old_c32[i] ? "Yes" : "No");
		}
	}
	uprintf("  Uses KolibriOS: %s", YesNo(iso_report.has_kolibrios));
	uprintf("  Uses ReactOS: %s", YesNo(IS_REACTOS(iso_report)));
	uprintf("  Uses WinPE: %s%s", YesNo(IS_WINPE(iso_report.winpe)), (iso_report.uses_minint) ? " (with /minint)" : "");

	// We don't support ToGo on Windows 7 or earlier, for lack of ISO mount capabilities
	// TODO: add install.wim extraction workaround for Windows 7
	if (nWindowsVersion >= WINDOWS_8)
		if ( ((!togo_mode) && (HAS_TOGO(iso_report))) || ((togo_mode) && (!HAS_TOGO(iso_report))) )
			ToggleToGo();
}

// The scanning process can be blocking for message processing => use a thread
DWORD WINAPI ISOScanThread(LPVOID param)
{
	int i;
	BOOL r;

	if (image_path == NULL)
		goto out;
	PrintInfoDebug(0, MSG_202);
	user_notified = FALSE;
	EnableControls(FALSE);
	r = ExtractISO(image_path, "", TRUE) || IsHDImage(image_path);
	EnableControls(TRUE);
	if (!r) {
		SendMessage(hMainDialog, UM_PROGRESS_EXIT, 0, 0);
		PrintInfoDebug(0, MSG_203);
		safe_free(image_path);
		PrintStatus(0, MSG_086);
		SetMBRProps();
		goto out;
	}

	if (iso_report.is_bootable_img) {
		uprintf("'%s' is a %sbootable %s image", image_path,
			(iso_report.compression_type != BLED_COMPRESSION_NONE)?"compressed ":"", iso_report.is_vhd?"VHD":"disk");
		selection_default = DT_IMG;
	} else {
		DisplayISOProps();
	}
	if ( (!iso_report.has_bootmgr) && (!HAS_SYSLINUX(iso_report)) && (!IS_WINPE(iso_report.winpe)) && (!IS_GRUB(iso_report))
	  && (!iso_report.has_efi) && (!IS_REACTOS(iso_report) && (!iso_report.has_kolibrios) && (!iso_report.is_bootable_img)) ) {
		PrintInfo(0, MSG_081);
		MessageBoxU(hMainDialog, lmprintf(MSG_082), lmprintf(MSG_081), MB_OK|MB_ICONINFORMATION|MB_IS_RTL);
		safe_free(image_path);
		PrintStatus(0, MSG_086);
		SetMBRProps();
	} else {
		// Enable bootable and set Target System and FS accordingly
		CheckDlgButton(hMainDialog, IDC_BOOT, BST_CHECKED);
		if (!iso_report.is_bootable_img) {
			SetTargetSystem();
			SetFSFromISO();
			SetMBRProps();
			// Some Linux distros, such as Arch Linux, require the USB drive to have
			// a specific label => copy the one we got from the ISO image
			if (iso_report.label[0] != 0) {
				SetWindowTextU(hLabel, iso_report.label);
			}
		} else {
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
				ComboBox_GetCurSel(hFileSystem));
		}
		for (i=(int)safe_strlen(image_path); (i>0)&&(image_path[i]!='\\'); i--);
		PrintStatusDebug(0, MSG_205, &image_path[i+1]);
		// Lose the focus on the select ISO (but place it on Close)
		SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)FALSE, 0);
		// Lose the focus from Close and set it back to Start
		SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hMainDialog, IDC_START), TRUE);
	}
	
	// Need to invalidate as we may have changed the UI and may get artifacts if we don't
	// Oh and we need to invoke BOTH RedrawWindow() and InvalidateRect() because UI refresh
	// in the Microsoft worlds SUCKS!!!! (we may lose the disabled "Start" button otherwise)
	RedrawWindow(hMainDialog, NULL, NULL, RDW_ALLCHILDREN | RDW_UPDATENOW);
	InvalidateRect(hMainDialog, NULL, TRUE);

out:
	PrintInfo(0, MSG_210);
	ExitThread(0);
}

// Move a control along the Y axis according to the advanced mode setting
static __inline void MoveCtrlY(HWND hDlg, int nID, float vertical_shift) {
	ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, nID), 0, (int)vertical_shift, 0, 0);
}

static void SetPassesTooltip(void)
{
	const unsigned char pattern[] = BADBLOCK_PATTERNS;
	CreateTooltip(hNBPasses, lmprintf(MSG_153 + ComboBox_GetCurSel(hNBPasses),
		pattern[0], pattern[1], pattern[2], pattern[3]), -1);
}

// Toggle "advanced" mode
static void ToggleAdvanced(void)
{
	float dialog_shift = 82.0f;
	RECT rect;
	POINT point;
	int toggle;

	advanced_mode = !advanced_mode;
	if (!advanced_mode)
		dialog_shift = -dialog_shift;

	// Increase or decrease the Window size
	GetWindowRect(hMainDialog, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top);
	MoveWindow(hMainDialog, rect.left, rect.top, point.x,
		point.y + (int)(fScale*dialog_shift), TRUE);

	// Move the status bar up or down
	MoveCtrlY(hMainDialog, IDC_STATUS, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_START, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_INFO, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_PROGRESS, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_ABOUT, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_LOG, dialog_shift);
	MoveCtrlY(hMainDialog, IDCANCEL, dialog_shift);
#ifdef RUFUS_TEST
	MoveCtrlY(hMainDialog, IDC_TEST, dialog_shift);
#endif

	// And do the same for the log dialog while we're at it
	GetWindowRect(hLogDlg, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top);
	MoveWindow(hLogDlg, rect.left, rect.top, point.x,
		point.y + (int)(fScale*dialog_shift), TRUE);
	MoveCtrlY(hLogDlg, IDC_LOG_CLEAR, dialog_shift);
	MoveCtrlY(hLogDlg, IDC_LOG_SAVE, dialog_shift);
	MoveCtrlY(hLogDlg, IDCANCEL, dialog_shift);
	GetWindowRect(hLog, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top) + (int)(fScale*dialog_shift);
	SetWindowPos(hLog, 0, 0, 0, point.x, point.y, 0);
	// Don't forget to scroll the edit to the bottom after resize
	SendMessage(hLog, EM_LINESCROLL, 0, SendMessage(hLog, EM_GETLINECOUNT, 0, 0));

	// Hide or show the various advanced options
	toggle = advanced_mode?SW_SHOW:SW_HIDE;
	ShowWindow(GetDlgItem(hMainDialog, IDC_ENABLE_FIXED_DISKS), toggle);
	ShowWindow(GetDlgItem(hMainDialog, IDC_EXTRA_PARTITION), toggle);
	ShowWindow(GetDlgItem(hMainDialog, IDC_RUFUS_MBR), toggle);
	ShowWindow(GetDlgItem(hMainDialog, IDC_DISK_ID), toggle);
	ShowWindow(GetDlgItem(hMainDialog, IDS_ADVANCED_OPTIONS_GRP), toggle);

	// Toggle the up/down icon
	SendMessage(GetDlgItem(hMainDialog, IDC_ADVANCED), BCM_SETIMAGELIST, 0, (LPARAM)(advanced_mode?&bi_up:&bi_down));

	// Never hurts to force Windows' hand
	InvalidateRect(hMainDialog, NULL, TRUE);
}

// Toggle DD Image mode
static void ToggleImage(BOOL enable)
{
	EnableWindow(GetDlgItem(hMainDialog, IDC_QUICKFORMAT), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_PARTITION_TYPE), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_FILESYSTEM), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_CLUSTERSIZE), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_LABEL), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_QUICKFORMAT), enable);
	EnableWindow(GetDlgItem(hMainDialog, IDC_SET_ICON), enable);
}

// Toggle the Windows To Go radio choice
static void ToggleToGo(void)
{
	float dialog_shift = 38.0f;
	RECT rect;
	POINT point;
	int toggle;

	// Windows To Go mode is only available for Windows 8 or later due to the lack
	// of an ISO mounting API on previous versions.
	// But we still need to be able to hide the Windows To Go option on startup.
	if ((nWindowsVersion < WINDOWS_8) && (!togo_mode))
		return;

	togo_mode = !togo_mode;
	if (!togo_mode)
		dialog_shift = -dialog_shift;

	// Increase or decrease the Window size
	GetWindowRect(hMainDialog, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top);
	MoveWindow(hMainDialog, rect.left, rect.top, point.x,
		point.y + (int)(fScale*dialog_shift), TRUE);

	// Move the controls up or down
	MoveCtrlY(hMainDialog, IDC_STATUS, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_START, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_INFO, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_PROGRESS, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_ABOUT, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_LOG, dialog_shift);
	MoveCtrlY(hMainDialog, IDCANCEL, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_SET_ICON, dialog_shift);
	MoveCtrlY(hMainDialog, IDS_ADVANCED_OPTIONS_GRP, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_ENABLE_FIXED_DISKS, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_EXTRA_PARTITION, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_RUFUS_MBR, dialog_shift);
	MoveCtrlY(hMainDialog, IDC_DISK_ID, dialog_shift);
	ResizeMoveCtrl(hMainDialog, GetDlgItem(hMainDialog, IDS_FORMAT_OPTIONS_GRP), 0, 0, 0, (int)dialog_shift);
	
#ifdef RUFUS_TEST
	MoveCtrlY(hMainDialog, IDC_TEST, dialog_shift);
#endif

	// And do the same for the log dialog while we're at it
	GetWindowRect(hLogDlg, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top);
	MoveWindow(hLogDlg, rect.left, rect.top, point.x,
		point.y + (int)(fScale*dialog_shift), TRUE);
	MoveCtrlY(hLogDlg, IDC_LOG_CLEAR, dialog_shift);
	MoveCtrlY(hLogDlg, IDC_LOG_SAVE, dialog_shift);
	MoveCtrlY(hLogDlg, IDCANCEL, dialog_shift);
	GetWindowRect(hLog, &rect);
	point.x = (rect.right - rect.left);
	point.y = (rect.bottom - rect.top) + (int)(fScale*dialog_shift);
	SetWindowPos(hLog, 0, 0, 0, point.x, point.y, 0);
	// Don't forget to scroll the edit to the bottom after resize
	SendMessage(hLog, EM_LINESCROLL, 0, SendMessage(hLog, EM_GETLINECOUNT, 0, 0));

	// Hide or show the various advanced options
	toggle = togo_mode?SW_SHOW:SW_HIDE;
	ShowWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL), toggle);
	ShowWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO), toggle);

	// Reset the radio button choice
	Button_SetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL), BST_CHECKED);
	Button_SetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO), BST_UNCHECKED);

	// If you don't force a redraw here, all kind of bad UI artifacts happen...
	InvalidateRect(hMainDialog, NULL, TRUE);
}

static BOOL BootCheck(void)
{
	int i, fs, bt, dt, pt, r;
	FILE *fd;
	DWORD len;
	BOOL in_files_dir = FALSE;
	const char* grub = "grub";
	const char* core_img = "core.img";
	const char* ldlinux = "ldlinux";
	const char* syslinux = "syslinux";
	const char* ldlinux_ext[3] = { "sys", "bss", "c32" };
	char tmp[MAX_PATH], tmp2[MAX_PATH];

	syslinux_ldlinux_len[0] = 0; syslinux_ldlinux_len[1] = 0;
	safe_free(grub2_buf);
	dt = (int)ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
	pt = GETPARTTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
	if ((dt == DT_ISO) || (dt == DT_IMG)) {
		if (image_path == NULL) {
			// Please click on the disc button to select a bootable ISO
			MessageBoxU(hMainDialog, lmprintf(MSG_087), lmprintf(MSG_086), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}
		if ((size_check) && (iso_report.projected_size > (uint64_t)SelectedDrive.DiskSize)) {
			// This ISO image is too big for the selected target
			MessageBoxU(hMainDialog, lmprintf(MSG_089), lmprintf(MSG_088), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}
		if (dt == DT_IMG) {
			if (!iso_report.is_bootable_img)
			// The selected image doesn't match the boot option selected.
				MessageBoxU(hMainDialog, lmprintf(MSG_188), lmprintf(MSG_187), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return (iso_report.is_bootable_img);
		}
		fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
		bt = GETBIOSTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
		if ((togo_mode) && (Button_GetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO)) == BST_CHECKED)) {
			if (fs != FS_NTFS) {
				// Windows To Go only works for NTFS
				MessageBoxU(hMainDialog, lmprintf(MSG_097, "Windows To Go"), lmprintf(MSG_092), MB_OK|MB_ICONERROR|MB_IS_RTL);
				return FALSE;
			} else if (SelectedDrive.Geometry.MediaType != FixedMedia) {
				if ((bt == BT_UEFI) && (pt == PARTITION_STYLE_GPT)) {
					// We're screwed since we need access to 2 partitions at the same time to set this, which
					// Windows can't do. Cue in Arthur's Theme: "♫ I know it's stupid... but it's true. ♫"
					MessageBoxU(hMainDialog, lmprintf(MSG_198), lmprintf(MSG_190), MB_OK|MB_ICONERROR|MB_IS_RTL);
					return FALSE;
				}
				// I never had any success with drives that have the REMOVABLE attribute set, no matter the
				// method or tool I tried. If you manage to get this working, I'd like to hear from you!
				if (MessageBoxU(hMainDialog, lmprintf(MSG_098), lmprintf(MSG_190), MB_YESNO|MB_ICONWARNING|MB_IS_RTL) != IDYES)
					return FALSE;
			}
		} else if (bt == BT_UEFI) {
			if (!iso_report.has_efi) {
				// Unsupported ISO
				MessageBoxU(hMainDialog, lmprintf(MSG_091), lmprintf(MSG_090), MB_OK|MB_ICONERROR|MB_IS_RTL);
				return FALSE;
			}
			if (IS_WIN7_EFI(iso_report) && (!WimExtractCheck())) {
				// Your platform cannot extract files from WIM archives => download 7-zip?
				if (MessageBoxU(hMainDialog, lmprintf(MSG_102), lmprintf(MSG_101), MB_YESNO|MB_ICONERROR|MB_IS_RTL) == IDYES)
					ShellExecuteA(hMainDialog, "open", SEVENZIP_URL, NULL, NULL, SW_SHOWNORMAL);
				return FALSE;
			}
		} else if ( ((fs == FS_NTFS) && (!iso_report.has_bootmgr) && (!IS_WINPE(iso_report.winpe)) && (!IS_GRUB(iso_report)))
				 || ((IS_FAT(fs)) && (!HAS_SYSLINUX(iso_report)) && (!allow_dual_uefi_bios) &&
					 (!IS_REACTOS(iso_report)) && (!iso_report.has_kolibrios) && (!IS_GRUB(iso_report))) ) {
			// Incompatible FS and ISO
			MessageBoxU(hMainDialog, lmprintf(MSG_096), lmprintf(MSG_092), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		} else if ((fs == FS_FAT16) && (iso_report.has_kolibrios)) {
			// KolibriOS doesn't support FAT16
			MessageBoxU(hMainDialog, lmprintf(MSG_189), lmprintf(MSG_099), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}
		if ((IS_FAT(fs)) && (iso_report.has_4GB_file)) {
			// This ISO image contains a file larger than 4GB file (FAT32)
			MessageBoxU(hMainDialog, lmprintf(MSG_100), lmprintf(MSG_099), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}

		if ((iso_report.has_grub2) && (iso_report.grub2_version[0] != 0) &&
			(strcmp(iso_report.grub2_version, GRUB2_PACKAGE_VERSION) != 0)) {
			// We may have to download a different Grub2 version if we can find one
			IGNORE_RETVAL(_chdirU(app_dir));
			IGNORE_RETVAL(_mkdir(FILES_DIR));
			IGNORE_RETVAL(_chdir(FILES_DIR));
			static_sprintf(tmp, "%s-%s/%s", grub, iso_report.grub2_version, core_img);
			fd = fopen(tmp, "rb");
			if (fd != NULL) {
				// If a file already exists in the current directory, use that one
				uprintf("Will reuse '%s' from './" FILES_DIR "/%s-%s/' for Grub 2.x installation\n",
					core_img, grub, iso_report.grub2_version);
				fseek(fd, 0, SEEK_END);
				grub2_len = ftell(fd);
				fseek(fd, 0, SEEK_SET);
				if (grub2_len > 0)
					grub2_buf = malloc(grub2_len);

				// grub2_buf was set to NULL at the beginning of this call
				if ((grub2_buf == NULL) || (fread(grub2_buf, 1, (size_t)grub2_len, fd) != (size_t)grub2_len)) {
					uprintf("Failed to read existing '%s' data - will use embedded version", core_img);
					safe_free(grub2_buf);
				}
				fclose(fd);
			} else {
				r = MessageBoxU(hMainDialog, lmprintf(MSG_116, iso_report.grub2_version, GRUB2_PACKAGE_VERSION),
					lmprintf(MSG_115), MB_YESNOCANCEL|MB_ICONWARNING|MB_IS_RTL);
				if (r == IDCANCEL)
					return FALSE;
				else if (r == IDYES) {
					static_sprintf(tmp, "%s-%s", grub, iso_report.grub2_version);
					IGNORE_RETVAL(_mkdir(tmp));
					static_sprintf(tmp, "%s/%s-%s/%s", FILES_URL, grub, iso_report.grub2_version, core_img);
					PrintInfoDebug(0, MSG_085, tmp);
					PromptOnError = FALSE;
					grub2_len = (long)DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog);
					PromptOnError = TRUE;
					if (grub2_len <= 0) {
						PrintInfo(0, MSG_195, "Grub2");
						uprintf("%s was not found - will use embedded version\n", tmp);
					} else {
						PrintInfo(0, MSG_193, tmp);
						fd = fopen(&tmp[sizeof(FILES_URL)], "rb");
						grub2_buf = malloc(grub2_len);
						if ((fd == NULL) || (grub2_buf == NULL) || (fread(grub2_buf, 1, (size_t)grub2_len, fd) != (size_t)grub2_len)) {
							uprintf("Failed to read '%s' data - will use embedded version", core_img);
							safe_free(grub2_buf);
						}
						if (fd != NULL)
							fclose(fd);
					}
				}
			}
		}

		if (HAS_SYSLINUX(iso_report)) {
			if (SL_MAJOR(iso_report.sl_version) < 5) {
				IGNORE_RETVAL(_chdirU(app_dir));
				for (i=0; i<NB_OLD_C32; i++) {
					if (iso_report.has_old_c32[i]) {
						if (!in_files_dir) {
							IGNORE_RETVAL(_mkdir(FILES_DIR));
							IGNORE_RETVAL(_chdir(FILES_DIR));
							in_files_dir = TRUE;
						}
						static_sprintf(tmp, "%s-%s/%s", syslinux, embedded_sl_version_str[0], old_c32_name[i]);
						fd = fopen(tmp, "rb");
						if (fd != NULL) {
							// If a file already exists in the current directory, use that one
							uprintf("Will replace obsolete '%s' from ISO with the one found in './" FILES_DIR "/%s'\n", old_c32_name[i], tmp);
							fclose(fd);
							use_own_c32[i] = TRUE;
						} else {
							PrintInfo(0, MSG_204, old_c32_name[i]);
							if (MessageBoxU(hMainDialog, lmprintf(MSG_084, old_c32_name[i], old_c32_name[i]),
									lmprintf(MSG_083, old_c32_name[i]), MB_YESNO|MB_ICONWARNING|MB_IS_RTL) == IDYES) {
								static_sprintf(tmp, "%s-%s", syslinux, embedded_sl_version_str[0]);
								IGNORE_RETVAL(_mkdir(tmp));
								static_sprintf(tmp, "%s/%s-%s/%s", FILES_URL, syslinux, embedded_sl_version_str[0], old_c32_name[i]);
								PrintInfo(0, MSG_085, old_c32_name[i]);
								len = DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog);
								if (len == 0) {
									uprintf("Could not download file - cancelling\n");
									return FALSE;
								}
								use_own_c32[i] = TRUE;
							}
						}
					}
				}
			} else if ((iso_report.sl_version != embedded_sl_version[1]) ||
				(safe_strcmp(iso_report.sl_version_ext, embedded_sl_version_ext[1]) != 0)) {
				// Unlike what was the case for v4 and earlier, Syslinux v5+ versions are INCOMPATIBLE with one another!
				IGNORE_RETVAL(_chdirU(app_dir));
				IGNORE_RETVAL(_mkdir(FILES_DIR));
				IGNORE_RETVAL(_chdir(FILES_DIR));
				for (i=0; i<2; i++) {
					// Check if we already have the relevant ldlinux_v#.##.sys & ldlinux_v#.##.bss files
					static_sprintf(tmp, "%s-%s%s/%s.%s", syslinux, iso_report.sl_version_str,
						iso_report.sl_version_ext, ldlinux, ldlinux_ext[i]);
					fd = fopen(tmp, "rb");
					if (fd != NULL) {
						fseek(fd, 0, SEEK_END);
						syslinux_ldlinux_len[i] = (DWORD)ftell(fd);
						fclose(fd);
					}
				}
				if ((syslinux_ldlinux_len[0] != 0) && (syslinux_ldlinux_len[1] != 0)) {
					uprintf("Will reuse '%s.%s' and '%s.%s' from './" FILES_DIR "/%s/%s-%s%s/' for Syslinux installation\n",
						ldlinux, ldlinux_ext[0], ldlinux, ldlinux_ext[1], FILES_DIR, syslinux,
						iso_report.sl_version_str, iso_report.sl_version_ext);
				} else {
					r = MessageBoxU(hMainDialog, lmprintf(MSG_114, iso_report.sl_version_str, iso_report.sl_version_ext,
						embedded_sl_version_str[1], embedded_sl_version_ext[1]),
						lmprintf(MSG_115), MB_YESNO|MB_ICONWARNING|MB_IS_RTL);
					if (r != IDYES)
						return FALSE;
					for (i=0; i<2; i++) {
						static_sprintf(tmp, "%s-%s", syslinux, iso_report.sl_version_str);
						IGNORE_RETVAL(_mkdir(tmp));
						if (*iso_report.sl_version_ext != 0) {
							IGNORE_RETVAL(_chdir(tmp));
							IGNORE_RETVAL(_mkdir(&iso_report.sl_version_ext[1]));
							IGNORE_RETVAL(_chdir(".."));
						}
						static_sprintf(tmp, "%s/%s-%s%s/%s.%s", FILES_URL, syslinux, iso_report.sl_version_str,
							iso_report.sl_version_ext, ldlinux, ldlinux_ext[i]);
						PrintInfo(0, MSG_085, tmp);
						PromptOnError = (*iso_report.sl_version_ext == 0);
						syslinux_ldlinux_len[i] = DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog);
						PromptOnError = TRUE;
						if ((syslinux_ldlinux_len[i] == 0) && (DownloadStatus == 404) && (*iso_report.sl_version_ext != 0)) {
							// Couldn't locate the file on the server => try to download without the version extra
							uprintf("Extended version was not found, trying main version\n");
							static_sprintf(tmp, "%s/%s-%s/%s.%s", FILES_URL, syslinux, iso_report.sl_version_str,
								ldlinux, ldlinux_ext[i]);
							PrintInfo(0, MSG_085, tmp);
							syslinux_ldlinux_len[i] = DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog);
							if (syslinux_ldlinux_len[i] != 0) {
								// Duplicate the file so that the user won't be prompted to download again
								static_sprintf(tmp, "%s-%s\\%s.%s", syslinux, iso_report.sl_version_str, ldlinux, ldlinux_ext[i]);
								static_sprintf(tmp2, "%s-%s\\%s\\%s.%s", syslinux, iso_report.sl_version_str, 
									&iso_report.sl_version_ext[1], ldlinux, ldlinux_ext[i]);
								CopyFileA(tmp, tmp2, FALSE);
							}
						}
						if (syslinux_ldlinux_len[i] == 0) {
							uprintf("Could not download the file - cancelling\n");
							return FALSE;
						}
					}
				}
			}
		}
	} else if (dt == DT_SYSLINUX_V6) {
		IGNORE_RETVAL(_chdirU(app_dir));
		IGNORE_RETVAL(_mkdir(FILES_DIR));
		IGNORE_RETVAL(_chdir(FILES_DIR));
		static_sprintf(tmp, "%s-%s/%s.%s", syslinux, embedded_sl_version_str[1], ldlinux, ldlinux_ext[2]);
		fd = fopenU(tmp, "rb");
		if (fd != NULL) {
			uprintf("Will reuse './%s/%s' for Syslinux installation\n", FILES_DIR, tmp);
			fclose(fd);
		} else {
			static_sprintf(tmp, "%s.%s", ldlinux, ldlinux_ext[2]);
			PrintInfo(0, MSG_206, tmp);
			// MSG_104: "Syslinux v5.0 or later requires a '%s' file to be installed"
			r = MessageBoxU(hMainDialog, lmprintf(MSG_104, "Syslinux v5.0", tmp, "Syslinux v5+", tmp),
				lmprintf(MSG_103, tmp), MB_YESNOCANCEL|MB_ICONWARNING|MB_IS_RTL);
			if (r == IDCANCEL)
				return FALSE;
			if (r == IDYES) {
				static_sprintf(tmp, "%s-%s", syslinux, embedded_sl_version_str[1]);
				IGNORE_RETVAL(_mkdir(tmp));
				static_sprintf(tmp, "%s/%s-%s/%s.%s", FILES_URL, syslinux, embedded_sl_version_str[1], ldlinux, ldlinux_ext[2]);
				PrintInfo(0, MSG_085, tmp);
				if (DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog) == 0)
					return FALSE;
			}
		}
	} else if (dt == DT_WINME) {
		if ((size_check) && (ComboBox_GetItemData(hClusterSize, ComboBox_GetCurSel(hClusterSize)) >= 65536)) {
			// MS-DOS cannot boot from a drive using a 64 kilobytes Cluster size
			MessageBoxU(hMainDialog, lmprintf(MSG_110), lmprintf(MSG_111), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}
	} else if (dt == DT_GRUB4DOS) {
		IGNORE_RETVAL(_chdirU(app_dir));
		IGNORE_RETVAL(_mkdir(FILES_DIR));
		IGNORE_RETVAL(_chdir(FILES_DIR));
		static_sprintf(tmp, "grub4dos-%s/grldr", GRUB4DOS_VERSION);
		fd = fopenU(tmp, "rb");
		if (fd != NULL) {
			uprintf("Will reuse './%s/%s' for Grub4DOS installation\n", FILES_DIR, tmp);
			fclose(fd);
		} else {
			static_sprintf(tmp, "grldr");
			PrintInfo(0, MSG_206, tmp);
			r = MessageBoxU(hMainDialog, lmprintf(MSG_104, "Grub4DOS 0.4", tmp, "Grub4DOS", tmp),
				lmprintf(MSG_103, tmp), MB_YESNOCANCEL|MB_ICONWARNING|MB_IS_RTL);
			if (r == IDCANCEL)
				return FALSE;
			if (r == IDYES) {
				static_sprintf(tmp, "grub4dos-%s", GRUB4DOS_VERSION);
				IGNORE_RETVAL(_mkdir(tmp));
				static_sprintf(tmp, "%s/grub4dos-%s/grldr", FILES_URL, GRUB4DOS_VERSION);
				PrintInfo(0, MSG_085, tmp);
				uprintf("URL = %s", tmp);
				if (DownloadFile(tmp, &tmp[sizeof(FILES_URL)], hMainDialog) == 0)
					return FALSE;
			}
		}
	} else if (dt == DT_UEFI_NTFS) {
		fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
		if (fs != FS_NTFS) {
			MessageBoxU(hMainDialog, lmprintf(MSG_097, "UEFI:NTFS"), lmprintf(MSG_092), MB_OK|MB_ICONERROR|MB_IS_RTL);
			return FALSE;
		}
	}
	return TRUE;
}

static __inline const char* IsAlphaOrBeta(void)
{
#if defined(ALPHA)
	return " (Alpha) ";
#elif defined(BETA)
	return " (Beta) ";
#else
	return " ";
#endif
}

static INT_PTR CALLBACK InfoCallback(HWND hCtrl, UINT message, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	RECT rect;
	PAINTSTRUCT ps;
	wchar_t winfo[128];

	switch (message) {

	// Prevent select (which screws up our display as it redraws the font using different settings)
	case WM_LBUTTONDOWN:
		return (INT_PTR)FALSE;

	// Prevent the select cursor from appearing
	case WM_SETCURSOR:
		SetCursor(LoadCursor(NULL, IDC_ARROW));
		return (INT_PTR)TRUE;

	// The things one needs to do to vertically center text in an edit control...
	case WM_PAINT:
		GetWindowTextW(hInfo, winfo, ARRAYSIZE(winfo));
		hdc = BeginPaint(hCtrl , &ps);
		SelectObject(hdc, hFont);
		SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
		SetTextAlign(hdc , TA_CENTER | TA_BASELINE);
		GetClientRect(hCtrl , &rect);
		// If you don't fill the client area, you get leftover text artifacts
		FillRect(hdc, &rect, CreateSolidBrush(GetSysColor(COLOR_BTNFACE)));
		TextOutW(hdc, rect.right/2, rect.bottom/2 + (int)(5.0f * fScale), winfo, (int)wcslen(winfo));
		EndPaint(hCtrl, &ps);
		return (INT_PTR)TRUE;
	}

	return CallWindowProc(info_original_proc, hCtrl, message, wParam, lParam);
}

void InitDialog(HWND hDlg)
{
	HINSTANCE hShell32DllInst, hUserLanguagesCplDllInst, hINetCplDllInst;
	HIMAGELIST hLangToolbarImageList;
	TBBUTTON tbLangToolbarButtons[1];
	RECT rcDeviceList, rcToolbarButton;
	DWORD len;
	SIZE sz;
	HWND hCtrl;
	HDC hDC;
	int i, i16, s16, lfHeight;
	char tmp[128], *token, *buf, *ext;
	wchar_t wtmp[128] = {0};
	static char* resource[2] = { MAKEINTRESOURCEA(IDR_SL_LDLINUX_V4_SYS), MAKEINTRESOURCEA(IDR_SL_LDLINUX_V6_SYS) };

#ifdef RUFUS_TEST
	ShowWindow(GetDlgItem(hDlg, IDC_TEST), SW_SHOW);
#endif

	PF_INIT(ImageList_Create, Comctl32);
	PF_INIT(ImageList_AddIcon, Comctl32);
	PF_INIT(ImageList_ReplaceIcon, Comctl32);

	// Quite a burden to carry around as parameters
	hMainDialog = hDlg;
	hDeviceList = GetDlgItem(hDlg, IDC_DEVICE);
	hPartitionScheme = GetDlgItem(hDlg, IDC_PARTITION_TYPE);
	hFileSystem = GetDlgItem(hDlg, IDC_FILESYSTEM);
	hClusterSize = GetDlgItem(hDlg, IDC_CLUSTERSIZE);
	hLabel = GetDlgItem(hDlg, IDC_LABEL);
	hProgress = GetDlgItem(hDlg, IDC_PROGRESS);
	hInfo = GetDlgItem(hDlg, IDC_INFO);
	hBoot = GetDlgItem(hDlg, IDC_BOOT);
	hBootType = GetDlgItem(hDlg, IDC_BOOTTYPE);
	hSelectISO = GetDlgItem(hDlg, IDC_SELECT_ISO);
	hNBPasses = GetDlgItem(hDlg, IDC_NBPASSES);
	hDiskID = GetDlgItem(hDlg, IDC_DISK_ID);

	// High DPI scaling
	i16 = GetSystemMetrics(SM_CXSMICON);
	hDC = GetDC(hDlg);
	fScale = GetDeviceCaps(hDC, LOGPIXELSX) / 96.0f;
	lfHeight = -MulDiv(9, GetDeviceCaps(hDC, LOGPIXELSY), 72);
	ReleaseDC(hDlg, hDC);
	// Adjust icon size lookup
	s16 = i16;
	if (s16 >= 54)
		s16 = 64;
	else if (s16 >= 40)
		s16 = 48;
	else if (s16 >= 28)
		s16 = 32;
	else if (s16 >= 20)
		s16 = 24;

	// Create the font for the Info edit box
	hFont = CreateFontA(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		0, 0, PROOF_QUALITY, 0, (nWindowsVersion >= WINDOWS_VISTA)?"Segoe UI":"Arial Unicode MS");

	// Create the title bar icon
	SetTitleBarIcon(hDlg);
	GetWindowTextA(hDlg, tmp, sizeof(tmp));
	// Count of Microsoft for making it more attractive to read a
	// version using strtok() than using GetFileVersionInfo()
	token = strtok(tmp, " ");
	for (i=0; (i<3) && ((token = strtok(NULL, ".")) != NULL); i++)
		rufus_version[i] = (uint16_t)atoi(token);

	// Redefine the title to be able to add "Alpha" or "Beta" and get the version in the right order for RTL
	if (!right_to_left_mode) {
		static_sprintf(tmp, APPLICATION_NAME " %d.%d.%d%s", rufus_version[0], rufus_version[1], rufus_version[2], IsAlphaOrBeta());
	} else {
		static_sprintf(tmp, "%s%d.%d.%d " APPLICATION_NAME, IsAlphaOrBeta(), rufus_version[0], rufus_version[1], rufus_version[2]);
	}
	SetWindowTextU(hDlg, tmp);
	uprintf(APPLICATION_NAME " version: %d.%d.%d%s", rufus_version[0], rufus_version[1], rufus_version[2], IsAlphaOrBeta());
	for (i=0; i<ARRAYSIZE(resource); i++) {
		buf = (char*)GetResource(hMainInstance, resource[i], _RT_RCDATA, "ldlinux_sys", &len, TRUE);
		if (buf == NULL) {
			uprintf("Warning: could not read embedded Syslinux v%d version", i+4);
		} else {
			embedded_sl_version[i] = GetSyslinuxVersion(buf, len, &ext);
			static_sprintf(embedded_sl_version_str[i], "%d.%02d", SL_MAJOR(embedded_sl_version[i]), SL_MINOR(embedded_sl_version[i]));
			safe_strcpy(embedded_sl_version_ext[i], sizeof(embedded_sl_version_ext[i]), ext);
			free(buf);
		}
	}
	uprintf("Windows version: %s", WindowsVersionStr);
	uprintf("Syslinux versions: %s%s, %s%s", embedded_sl_version_str[0], embedded_sl_version_ext[0],
		embedded_sl_version_str[1], embedded_sl_version_ext[1]);
	uprintf("Grub versions: %s, %s", GRUB4DOS_VERSION, GRUB2_PACKAGE_VERSION);
	uprintf("Locale ID: 0x%04X", GetUserDefaultUILanguage());

	SetClusterSizeLabels();

	// Prefer FreeDOS to MS-DOS
	selection_default = DT_FREEDOS;
	// Create the status line and initialize the taskbar icon for progress overlay
	CreateStatusBar();
	CreateTaskbarList();
	SetTaskbarProgressState(TASKBAR_NORMAL);

	// Use maximum granularity for the progress bar
	SendMessage(hProgress, PBM_SETRANGE, 0, (MAX_PROGRESS<<16) & 0xFFFF0000);
	// Fill up the passes
	for (i=0; i<4; i++) {
		IGNORE_RETVAL(ComboBox_AddStringU(hNBPasses, lmprintf((i==0)?MSG_034:MSG_035, i+1)));
	}
	IGNORE_RETVAL(ComboBox_SetCurSel(hNBPasses, 0));
	SetPassesTooltip();
	// Fill up the boot type dropdown
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "MS-DOS"), DT_WINME));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "FreeDOS"), DT_FREEDOS));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, lmprintf(MSG_036)), DT_ISO));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, lmprintf(MSG_095)), DT_IMG));
	IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, selection_default));
	// Fill up the MBR masqueraded disk IDs ("8 disks should be enough for anybody")
	IGNORE_RETVAL(ComboBox_SetItemData(hDiskID, ComboBox_AddStringU(hDiskID, lmprintf(MSG_030, LEFT_TO_RIGHT_MARK "0x80")), 0x80));
	for (i=1; i<=7; i++) {
		IGNORE_RETVAL(ComboBox_SetItemData(hDiskID, ComboBox_AddStringU(hDiskID, lmprintf(MSG_109, 0x80+i, i+1)), 0x80+i));
	}
	IGNORE_RETVAL(ComboBox_SetCurSel(hDiskID, 0));

	// Create the string array
	StrArrayCreate(&DriveID, MAX_DRIVES);
	StrArrayCreate(&DriveLabel, MAX_DRIVES);
	// Set various checkboxes
	CheckDlgButton(hDlg, IDC_QUICKFORMAT, BST_CHECKED);
	CheckDlgButton(hDlg, IDC_BOOT, BST_CHECKED);
	CheckDlgButton(hDlg, IDC_SET_ICON, BST_CHECKED);

	// Load system icons (NB: Use the excellent http://www.nirsoft.net/utils/iconsext.html to find icon IDs)
	hShell32DllInst = GetLibraryHandle("Shell32");
	hIconDisc = (HICON)LoadImage(hShell32DllInst, MAKEINTRESOURCE(12), IMAGE_ICON, s16, s16, LR_DEFAULTCOLOR | LR_SHARED);

	if (nWindowsVersion >= WINDOWS_8) {
		// Use the icon from the Windows 8+ 'Language' Control Panel
		hUserLanguagesCplDllInst = GetLibraryHandle("UserLanguagesCpl");
		hIconLang = (HICON)LoadImage(hUserLanguagesCplDllInst, MAKEINTRESOURCE(1), IMAGE_ICON, s16, s16, LR_DEFAULTCOLOR | LR_SHARED);
	} else {
		// Otherwise use the globe icon, from the Internet Options Control Panel
		hINetCplDllInst = GetLibraryHandle("inetcpl.cpl");
		hIconLang = (HICON)LoadImage(hINetCplDllInst, MAKEINTRESOURCE(1313), IMAGE_ICON, s16, s16, LR_DEFAULTCOLOR | LR_SHARED);
	}

	if (nWindowsVersion >= WINDOWS_VISTA) {
		hIconDown = (HICON)LoadImage(hShell32DllInst, MAKEINTRESOURCE(16750), IMAGE_ICON, s16, s16, LR_DEFAULTCOLOR | LR_SHARED);
		hIconUp = (HICON)LoadImage(hShell32DllInst, MAKEINTRESOURCE(16749), IMAGE_ICON, s16, s16, LR_DEFAULTCOLOR | LR_SHARED);
	} else {
		hIconDown = (HICON)LoadImage(hMainInstance, MAKEINTRESOURCE(IDI_DOWN), IMAGE_ICON, 16, 16, 0);
		hIconUp = (HICON)LoadImage(hMainInstance, MAKEINTRESOURCE(IDI_UP), IMAGE_ICON, 16, 16, 0);
	}

	// Create the language toolbar
	// NB: We don't make it a tabstop as it would become the default selected button otherwise
	hLangToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, NULL, WS_CHILD | WS_TABSTOP | TBSTYLE_TRANSPARENT | CCS_NOPARENTALIGN |
		CCS_NORESIZE | CCS_NODIVIDER, 0, 0, 0, 0, hMainDialog, NULL, hMainInstance, NULL);
	if ((pfImageList_Create != NULL) && (pfImageList_AddIcon != NULL)) {
		hLangToolbarImageList = pfImageList_Create(i16, i16, ILC_COLOR32, 1, 0);
		pfImageList_AddIcon(hLangToolbarImageList, hIconLang);
		SendMessage(hLangToolbar, TB_SETIMAGELIST, (WPARAM)0, (LPARAM)hLangToolbarImageList);
	}
	SendMessage(hLangToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
	memset(tbLangToolbarButtons, 0, sizeof(TBBUTTON));
	tbLangToolbarButtons[0].idCommand = lang_button_id;
	tbLangToolbarButtons[0].fsStyle = BTNS_WHOLEDROPDOWN;
	tbLangToolbarButtons[0].fsState = TBSTATE_ENABLED;
	SendMessage(hLangToolbar, TB_ADDBUTTONS, (WPARAM)1, (LPARAM)&tbLangToolbarButtons); // Add just the 1 button
	SendMessage(hLangToolbar, TB_GETRECT, lang_button_id, (LPARAM)&rcToolbarButton);
	
	// Make the toolbar window just big enough to hold the button
	// Set the top margin to 4 DIPs and the right margin so that it's aligned with the Device List Combobox
	GetWindowRect(hDeviceList, &rcDeviceList);
	MapWindowPoints(NULL, hDlg, (POINT*)&rcDeviceList, 2);
	SetWindowPos(hLangToolbar, NULL, rcDeviceList.right - rcToolbarButton.right,
		(int)(4.0f * fScale), rcToolbarButton.right, rcToolbarButton.bottom, 0);
	ShowWindow(hLangToolbar, SW_SHOWNORMAL);

	// Reposition the Advanced button
	hCtrl = GetDlgItem(hDlg, IDS_FORMAT_OPTIONS_GRP);
	sz = GetTextSize(hCtrl);
	ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_ADVANCED), (int)((1.0f * sz.cx) / fScale), 0, 0, 0);
	// Add a space to the "Format Options" text
	GetWindowTextW(hCtrl, wtmp, ARRAYSIZE(wtmp));
	wtmp[wcslen(wtmp)] = ' ';
	SetWindowTextW(hCtrl, wtmp);
	// The things one needs to do to keep things looking good...
	if (nWindowsVersion == WINDOWS_7) {
		ResizeMoveCtrl(hDlg, GetDlgItem(hMainDialog, IDS_ADVANCED_OPTIONS_GRP), 0, -1, 0, 2);
		ResizeMoveCtrl(hDlg, hProgress, 0, 1, 0, 0);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_ADVANCED), -1, 0, 0, 0);
	}

	// Subclass the Info box so that we can align its text vertically
	info_original_proc = (WNDPROC)SetWindowLongPtr(hInfo, GWLP_WNDPROC, (LONG_PTR)InfoCallback);

	// Set the icons on the the buttons
	if ((pfImageList_Create != NULL) && (pfImageList_ReplaceIcon != NULL)) {

		bi_iso.himl = pfImageList_Create(i16, i16, ILC_COLOR32 | ILC_MASK, 1, 0);
		pfImageList_ReplaceIcon(bi_iso.himl, -1, hIconDisc);
		SetRect(&bi_iso.margin, 0, 1, 0, 0);
		bi_iso.uAlign = BUTTON_IMAGELIST_ALIGN_CENTER;
		bi_down.himl = pfImageList_Create(i16, i16, ILC_COLOR32 | ILC_MASK, 1, 0);
		pfImageList_ReplaceIcon(bi_down.himl, -1, hIconDown);
		SetRect(&bi_down.margin, 0, 0, 0, 0);
		bi_down.uAlign = BUTTON_IMAGELIST_ALIGN_CENTER;
		bi_up.himl = pfImageList_Create(i16, i16, ILC_COLOR32 | ILC_MASK, 1, 0);
		pfImageList_ReplaceIcon(bi_up.himl, -1, hIconUp);
		SetRect(&bi_up.margin, 0, 0, 0, 0);
		bi_up.uAlign = BUTTON_IMAGELIST_ALIGN_CENTER;

		SendMessage(hSelectISO, BCM_SETIMAGELIST, 0, (LPARAM)&bi_iso);
		SendMessage(GetDlgItem(hDlg, IDC_ADVANCED), BCM_SETIMAGELIST, 0, (LPARAM)&bi_down);
	}

	// Set the various tooltips
	CreateTooltip(hFileSystem, lmprintf(MSG_157), -1);
	CreateTooltip(hClusterSize, lmprintf(MSG_158), -1);
	CreateTooltip(hLabel, lmprintf(MSG_159), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ADVANCED), lmprintf(MSG_160), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_BADBLOCKS), lmprintf(MSG_161), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_QUICKFORMAT), lmprintf(MSG_162), -1);
	CreateTooltip(hBoot, lmprintf(MSG_163), -1);
	CreateTooltip(hBootType, lmprintf(MSG_164), -1);
	CreateTooltip(hSelectISO, lmprintf(MSG_165), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_SET_ICON), lmprintf(MSG_166), 10000);
	CreateTooltip(GetDlgItem(hDlg, IDC_RUFUS_MBR), lmprintf(MSG_167), 10000);
	CreateTooltip(hDiskID, lmprintf(MSG_168), 10000);
	CreateTooltip(GetDlgItem(hDlg, IDC_EXTRA_PARTITION), lmprintf(MSG_169), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ENABLE_FIXED_DISKS), lmprintf(MSG_170), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_START), lmprintf(MSG_171), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_ABOUT), lmprintf(MSG_172), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_WINDOWS_INSTALL), lmprintf(MSG_199), -1);
	CreateTooltip(GetDlgItem(hDlg, IDC_WINDOWS_TO_GO), lmprintf(MSG_200), -1);

	ToggleAdvanced();	// We start in advanced mode => go to basic mode
	ToggleToGo();

	// Process commandline parameters
	if (iso_provided) {
		// Simulate a button click for ISO selection
		PostMessage(hDlg, WM_COMMAND, IDC_SELECT_ISO, 0);
	}

	PrintInfo(0, MSG_210);
}

static void PrintStatus2000(const char* str, BOOL val)
{
	PrintStatus(2000, (val)?MSG_250:MSG_251, str);
}

void ShowLanguageMenu(RECT rcExclude)
{
	TPMPARAMS tpm;
	HMENU menu;
	loc_cmd* lcmd = NULL;
	char lang[256];
	char *search = "()";
	char *l, *r, *str;

	UM_LANGUAGE_MENU_MAX = UM_LANGUAGE_MENU;
	menu = CreatePopupMenu();
	list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
		// The appearance of LTR languages must be fixed for RTL menus
		if ((right_to_left_mode) && (!(lcmd->ctrl_id & LOC_RIGHT_TO_LEFT)))  {
			str = safe_strdup(lcmd->txt[1]);
			l = strtok(str, search);
			r = strtok(NULL, search);
			static_sprintf(lang, LEFT_TO_RIGHT_MARK "(%s) " LEFT_TO_RIGHT_MARK "%s", r, l);
			safe_free(str);
		} else {
			safe_strcpy(lang, sizeof(lang), lcmd->txt[1]);
		}
		InsertMenuU(menu, -1, MF_BYPOSITION|((selected_locale == lcmd)?MF_CHECKED:0), UM_LANGUAGE_MENU_MAX++, lang);
	}

	// Open the menu such that it doesn't overlap the specified rect
	tpm.cbSize = sizeof(TPMPARAMS);
	tpm.rcExclude = rcExclude;
	TrackPopupMenuEx(menu, 0,
		right_to_left_mode ? rcExclude.right : rcExclude.left, // In RTL languages, the menu should be placed at the bottom-right of the rect
		rcExclude.bottom, hMainDialog, &tpm);

	DestroyMenu(menu);
}

void SetBoot(int fs, int bt)
{
	int i;
	char tmp[32];

	IGNORE_RETVAL(ComboBox_ResetContent(hBootType));
	if ((bt == BT_BIOS) && (IS_FAT(fs))) {
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "MS-DOS"), DT_WINME));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "FreeDOS"), DT_FREEDOS));
	}
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, lmprintf(MSG_036)), DT_ISO));
	IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, lmprintf(MSG_095)), DT_IMG));
	// If needed (advanced mode) also append "bare" Syslinux and other options
	if ( (bt == BT_BIOS) && ((IS_FAT(fs) || (fs == FS_NTFS)) && (advanced_mode)) ) {
		static_sprintf(tmp, "Syslinux %s", embedded_sl_version_str[0]);
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, tmp), DT_SYSLINUX_V4));
		static_sprintf(tmp, "Syslinux %s", embedded_sl_version_str[1]);
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, tmp), DT_SYSLINUX_V6));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "ReactOS"), DT_REACTOS));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType,
			"Grub " GRUB2_PACKAGE_VERSION), DT_GRUB2));
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType,
			"Grub4DOS " GRUB4DOS_VERSION), DT_GRUB4DOS));
	}
	if (advanced_mode)
		IGNORE_RETVAL(ComboBox_SetItemData(hBootType, ComboBox_AddStringU(hBootType, "UEFI:NTFS"), DT_UEFI_NTFS));
	if ((!advanced_mode) && (selection_default >= DT_SYSLINUX_V4)) {
		selection_default = DT_FREEDOS;
		CheckDlgButton(hMainDialog, IDC_DISK_ID, BST_UNCHECKED);
	}
	for (i=0; i<ComboBox_GetCount(hBootType); i++) {
		if (ComboBox_GetItemData(hBootType, i) == selection_default) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, i));
			break;
		}
	}
	if (i == ComboBox_GetCount(hBootType))
		IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, 0));

	if (!IsWindowEnabled(hBoot)) {
		EnableWindow(hBoot, TRUE);
		EnableWindow(hBootType, TRUE);
		EnableWindow(hSelectISO, TRUE);
		EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL), TRUE);
		EnableWindow(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO), TRUE);
		CheckDlgButton(hMainDialog, IDC_BOOT, uBootChecked);
	}
}

/*
 * Main dialog callback
 */
static INT_PTR CALLBACK MainCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static DWORD DeviceNum = 0, LastRefresh = 0;
	static BOOL first_log_display = TRUE, user_changed_label = FALSE, isMarquee = FALSE;
	static ULONG ulRegister = 0;
	static LPITEMIDLIST pidlDesktop = NULL;
	static MY_SHChangeNotifyEntry NotifyEntry;
	DRAWITEMSTRUCT* pDI;
	POINT Point;
	RECT DialogRect, DesktopRect, LangToolbarRect;
	LONG progress_style;
	int nDeviceIndex, fs, bt, i, nWidth, nHeight, nb_devices, selected_language, offset;
	char tmp[128];
	loc_cmd* lcmd = NULL;
	EXT_DECL(img_ext, NULL, __VA_GROUP__("*.img;*.vhd;*.gz;*.bzip2;*.xz;*.lzma;*.Z"), __VA_GROUP__(lmprintf(MSG_095)));
	EXT_DECL(iso_ext, NULL, __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));
	LPNMTOOLBAR lpnmtb;

	switch (message) {

	case UM_MEDIA_CHANGE:
		wParam = DBT_CUSTOMEVENT;
		// Fall through
	case WM_DEVICECHANGE:
		// The Windows hotplug subsystem sucks. Among other things, if you insert a GPT partitioned
		// USB drive with zero partitions, the only device messages you will get are a stream of
		// DBT_DEVNODES_CHANGED and that's it. But those messages are also issued when you get a
		// DBT_DEVICEARRIVAL and DBT_DEVICEREMOVECOMPLETE, and there's a whole slew of them so we
		// can't really issue a refresh for each one we receive
		// What we do then is arm a timer on DBT_DEVNODES_CHANGED, if it's been more than 1 second
		// since last refresh/arm timer, and have that timer send DBT_CUSTOMEVENT when it expires.
		// DO *NOT* USE WM_DEVICECHANGE AS THE MESSAGE FROM THE TIMER PROC, as it may be filtered!
		// For instance filtering will occur when (un)plugging in a FreeBSD UFD on Windows 8.
		// Instead, use a custom user message, such as UM_MEDIA_CHANGE, to set DBT_CUSTOMEVENT.
		if (format_thid == NULL) {
			switch (wParam) {
			case DBT_DEVICEARRIVAL:
			case DBT_DEVICEREMOVECOMPLETE:
			case DBT_CUSTOMEVENT:	// Sent by our timer refresh function or for card reader media change
				LastRefresh = GetTickCount();	// Don't care about 49.7 days rollback of GetTickCount()
				KillTimer(hMainDialog, TID_REFRESH_TIMER);
				GetUSBDevices((DWORD)ComboBox_GetItemData(hDeviceList, ComboBox_GetCurSel(hDeviceList)));
				user_changed_label = FALSE;
				return (INT_PTR)TRUE;
			case DBT_DEVNODES_CHANGED:
				// If it's been more than a second since last device refresh, arm a refresh timer
				if (GetTickCount() > LastRefresh + 1000) {
					LastRefresh = GetTickCount();
					SetTimer(hMainDialog, TID_REFRESH_TIMER, 1000, RefreshTimer);
				}
				break;
			default:
				break;
			}
		}
		break;

	case WM_INITDIALOG:
		PF_INIT(SHChangeNotifyRegister, shell32);
		apply_localization(IDD_DIALOG, hDlg);
		SetUpdateCheck();
		advanced_mode = TRUE;
		togo_mode = TRUE;
		// Create the log window (hidden)
		first_log_display = TRUE;
		log_displayed = FALSE;
		hLogDlg = CreateDialogW(hMainInstance, MAKEINTRESOURCEW(IDD_LOG + IDD_OFFSET), hDlg, (DLGPROC)LogProc);
		InitDialog(hDlg);
		GetUSBDevices(0);
		CheckForUpdates(FALSE);
		// Register MEDIA_INSERTED/MEDIA_REMOVED notifications for card readers
		if ((pfSHChangeNotifyRegister != NULL) && (SUCCEEDED(SHGetSpecialFolderLocation(0, CSIDL_DESKTOP, &pidlDesktop)))) {
			NotifyEntry.pidl = pidlDesktop;
			NotifyEntry.fRecursive = TRUE;
			// NB: The following only works if the media is already formatted.
			// If you insert a blank card, notifications will not be sent... :(
			ulRegister = pfSHChangeNotifyRegister(hDlg, 0x0001|0x0002|0x8000,
				SHCNE_MEDIAINSERTED|SHCNE_MEDIAREMOVED, UM_MEDIA_CHANGE, 1, &NotifyEntry);
		}
		// Bring our Window on top. We have to go through all *THREE* of these, or Far Manager hides our window :(
		SetWindowPos(hMainDialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE);
		SetWindowPos(hMainDialog, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE);
		SetWindowPos(hMainDialog, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE|SWP_NOMOVE);

		// Set 'Start' as the selected button if it's enabled, otherwise use 'Close', instead
		SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)(IsWindowEnabled(GetDlgItem(hDlg, IDC_START)) ? GetDlgItem(hDlg, IDC_START) : GetDlgItem(hDlg, IDCANCEL)), TRUE);


#if defined(ALPHA)
		// Add a VERY ANNOYING popup for Alpha releases, so that people don't start redistributing them
		Notification(MSG_INFO, NULL, "ALPHA VERSION", "This is an Alpha version of " APPLICATION_NAME
			" - It is meant to be used for testing ONLY and should NOT be distributed as a release.");
#endif
		return (INT_PTR)FALSE;

	// The things one must do to get an ellipsis on the status bar...
	case WM_DRAWITEM:
		if (wParam == IDC_STATUS) {
			pDI = (DRAWITEMSTRUCT*)lParam;
			pDI->rcItem.top += (int)(2.0f * fScale);
			pDI->rcItem.left += (int)(4.0f * fScale);
			SetBkMode(pDI->hDC, TRANSPARENT);
			switch(pDI->itemID) {
			case 0:	// left part
				DrawTextExU(pDI->hDC, szStatusMessage, -1, &pDI->rcItem,
					DT_LEFT|DT_END_ELLIPSIS|DT_PATH_ELLIPSIS, NULL);
				return (INT_PTR)TRUE;
			case 1:	// right part
				SetTextColor(pDI->hDC, GetSysColor(COLOR_3DSHADOW));
				DrawTextExA(pDI->hDC, szTimer, -1, &pDI->rcItem, DT_LEFT, NULL);
				return (INT_PTR)TRUE;
			}
		}
		break;

	case WM_COMMAND:
		if ((LOWORD(wParam) >= UM_LANGUAGE_MENU) && (LOWORD(wParam) < UM_LANGUAGE_MENU_MAX)) {
			selected_language = LOWORD(wParam) - UM_LANGUAGE_MENU;
			i = 0;
			list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
				if (i++ == selected_language) {
					if (selected_locale != lcmd) {
						selected_locale = lcmd;
						relaunch = TRUE;
						PostMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
					}
					break;
				}
			}
		}
		switch(LOWORD(wParam)) {
		case IDOK:			// close application
		case IDCANCEL:
			PF_INIT(SHChangeNotifyDeregister, Shell32);
			EnableWindow(GetDlgItem(hDlg, IDCANCEL), FALSE);
			if (format_thid != NULL) {
				if (MessageBoxU(hMainDialog, lmprintf(MSG_105), lmprintf(MSG_049),
					MB_YESNO|MB_ICONWARNING|MB_IS_RTL) == IDYES) {
					// Operation may have completed in the meantime
					if (format_thid != NULL) {
						FormatStatus = ERROR_SEVERITY_ERROR|FAC(FACILITY_STORAGE)|ERROR_CANCELLED;
						PrintInfo(0, MSG_201);
						uprintf("Cancelling");
						//  Start a timer to detect blocking operations during ISO file extraction
						if (iso_blocking_status >= 0) {
							last_iso_blocking_status = iso_blocking_status;
							SetTimer(hMainDialog, TID_BLOCKING_TIMER, 3000, BlockingTimer);
						}
					}
				} else {
					EnableWindow(GetDlgItem(hDlg, IDCANCEL), TRUE);
				}
				return (INT_PTR)TRUE;
			}
			if ((pfSHChangeNotifyDeregister != NULL) && (ulRegister != 0))
				pfSHChangeNotifyDeregister(ulRegister);
			PostQuitMessage(0);
			StrArrayDestroy(&DriveID);
			StrArrayDestroy(&DriveLabel);
			DestroyAllTooltips();
			DestroyWindow(hLogDlg);
			GetWindowRect(hDlg, &relaunch_rc);
			EndDialog(hDlg, 0);
			break;
		case IDC_ABOUT:
			CreateAboutBox();
			break;
		case IDC_LOG:
			// Place the log Window to the right (or left for RTL) of our dialog on first display
			if (first_log_display) {
				GetClientRect(GetDesktopWindow(), &DesktopRect);
				GetWindowRect(hLogDlg, &DialogRect);
				nWidth = DialogRect.right - DialogRect.left;
				nHeight = DialogRect.bottom - DialogRect.top;
				GetWindowRect(hDlg, &DialogRect);
				offset = GetSystemMetrics(SM_CXSIZEFRAME) + (int)(2.0f * fScale);
				if (nWindowsVersion >= WINDOWS_10)
					offset += (int)(-14.0f * fScale);
				if (right_to_left_mode)
					Point.x = max(DialogRect.left - offset - nWidth, 0);
				else
					Point.x = min(DialogRect.right + offset, DesktopRect.right - nWidth);
				
				Point.y = max(DialogRect.top, DesktopRect.top - nHeight);
				MoveWindow(hLogDlg, Point.x, Point.y, nWidth, nHeight, FALSE);
				// The log may have been recentered to fit the screen, in which case, try to shift our main dialog left (or right for RTL)
				nWidth = DialogRect.right - DialogRect.left;
				nHeight = DialogRect.bottom - DialogRect.top;
				if (right_to_left_mode) {
					Point.x = DialogRect.left;
					GetWindowRect(hLogDlg, &DialogRect);
					Point.x = max(Point.x, DialogRect.right - DialogRect.left + offset);
				} else {
					Point.x = max((DialogRect.left<0)?DialogRect.left:0, Point.x - offset - nWidth);
				}
				MoveWindow(hDlg, Point.x, Point.y, nWidth, nHeight, TRUE);
				first_log_display = FALSE;
			}
			// Display the log Window
			log_displayed = !log_displayed;
			// Set focus on the start button
			SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)FALSE, 0);
			SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hMainDialog, IDC_START), TRUE);
			// Must come last for the log window to get focus
			ShowWindow(hLogDlg, log_displayed?SW_SHOW:SW_HIDE);
			break;
#ifdef RUFUS_TEST
		case IDC_TEST:
			if (format_thid != NULL) {
				return (INT_PTR)TRUE;
			}
			FormatStatus = 0;
			format_op_in_progress = TRUE;
			// Reset all progress bars
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
			SetTaskbarProgressState(TASKBAR_NORMAL);
			SetTaskbarProgressValue(0, MAX_PROGRESS);
			SendMessage(hProgress, PBM_SETPOS, 0, 0);
			nDeviceIndex = ComboBox_GetCurSel(hDeviceList);
			if (nDeviceIndex != CB_ERR) {
				if ((IsChecked(IDC_BOOT)) && (!BootCheck())) {
					format_op_in_progress = FALSE;
					break;
				}

				GetWindowTextU(hDeviceList, tmp, ARRAYSIZE(tmp));
				if (MessageBoxU(hMainDialog, lmprintf(MSG_003, tmp),
					APPLICATION_NAME, MB_OKCANCEL|MB_ICONWARNING|MB_IS_RTL) == IDCANCEL) {
					format_op_in_progress = FALSE;
					break;
				}
				safe_free(image_path);
				image_path = strdup("C:\\Downloads\\my.vhd");

				// Disable all controls except cancel
				EnableControls(FALSE);
				DeviceNum = (DWORD)ComboBox_GetItemData(hDeviceList, nDeviceIndex);
				FormatStatus = 0;
				InitProgress(TRUE);
				format_thid = CreateThread(NULL, 0, SaveImageThread, (LPVOID)(uintptr_t)DeviceNum, 0, NULL);
				if (format_thid == NULL) {
					uprintf("Unable to start saving thread");
					FormatStatus = ERROR_SEVERITY_ERROR|FAC(FACILITY_STORAGE)|APPERR(ERROR_CANT_START_THREAD);
					PostMessage(hMainDialog, UM_FORMAT_COMPLETED, 0, 0);
				}
				uprintf("\r\nSave to image operation started");
				PrintInfo(0, -1);
				timer = 0;
				safe_sprintf(szTimer, sizeof(szTimer), "00:00:00");
				SendMessageA(GetDlgItem(hMainDialog, IDC_STATUS), SB_SETTEXTA,
					SBT_OWNERDRAW | 1, (LPARAM)szTimer);
				SetTimer(hMainDialog, TID_APP_TIMER, 1000, ClockTimer);
			}
			if (format_thid == NULL)
				format_op_in_progress = FALSE;
			break;
#endif
		case IDC_ADVANCED:
			ToggleAdvanced();
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_LABEL:
			if (HIWORD(wParam) == EN_CHANGE)
				user_changed_label = TRUE;
			break;
		case IDC_DEVICE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			nb_devices = ComboBox_GetCount(hDeviceList);
			PrintStatusDebug(0, (nb_devices==1)?MSG_208:MSG_209, nb_devices);
			PopulateProperties(ComboBox_GetCurSel(hDeviceList));
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
				ComboBox_GetCurSel(hFileSystem));
			break;
		case IDC_NBPASSES:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetPassesTooltip();
			break;
		case IDC_PARTITION_TYPE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			SetPartitionSchemeTooltip();
			SetFSFromISO();
			// fall-through
		case IDC_FILESYSTEM:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
			bt = GETBIOSTYPE((int)ComboBox_GetItemData(hPartitionScheme, ComboBox_GetCurSel(hPartitionScheme)));
			if ((selection_default == DT_IMG) && IsChecked(IDC_BOOT)) {
				ToggleImage(FALSE);
				EnableAdvancedBootOptions(FALSE, TRUE);
				SetBoot(fs, bt);
				SetToGo();
				break;
			}
			SetClusterSizes(fs);
			// Disable/restore the quick format control depending on large FAT32 or ReFS
			if ( ((fs == FS_FAT32) && ((SelectedDrive.DiskSize > LARGE_FAT32_SIZE) || (force_large_fat32))) || (fs == FS_REFS) ) {
				if (IsWindowEnabled(GetDlgItem(hMainDialog, IDC_QUICKFORMAT))) {
					uQFChecked = IsChecked(IDC_QUICKFORMAT);
					CheckDlgButton(hMainDialog, IDC_QUICKFORMAT, BST_CHECKED);
					EnableWindow(GetDlgItem(hMainDialog, IDC_QUICKFORMAT), FALSE);
				}
			} else {
				if (!IsWindowEnabled(GetDlgItem(hMainDialog, IDC_QUICKFORMAT))) {
					CheckDlgButton(hMainDialog, IDC_QUICKFORMAT, uQFChecked);
					EnableWindow(GetDlgItem(hMainDialog, IDC_QUICKFORMAT), TRUE);
				}
			}
			if (fs < 0) {
				EnableBootOptions(TRUE, TRUE);
				SetMBRProps();
				// Remove the SysLinux and ReactOS options if they exists
				if (ComboBox_GetItemData(hBootType, ComboBox_GetCount(hBootType)-1) == (DT_MAX-1)) {
					for (i=DT_SYSLINUX_V4; i<DT_MAX; i++)
						IGNORE_RETVAL(ComboBox_DeleteString(hBootType,  ComboBox_GetCount(hBootType)-1));
					IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, 1));
				}
				break;
			}
			if ((fs == FS_EXFAT) || (fs == FS_UDF) || (fs == FS_REFS)) {
				if (IsWindowEnabled(hBoot)) {
					// unlikely to be supported by BIOSes => don't bother
					IGNORE_RETVAL(ComboBox_SetCurSel(hBootType, 0));
					uBootChecked = IsChecked(IDC_BOOT);
					CheckDlgButton(hDlg, IDC_BOOT, BST_UNCHECKED);
					EnableBootOptions(FALSE, TRUE);
				} else if (IsChecked(IDC_BOOT)) {
					uBootChecked = TRUE;
					CheckDlgButton(hDlg, IDC_BOOT, BST_UNCHECKED);
				}
				SetMBRProps();
				break;
			}
			EnableAdvancedBootOptions(TRUE, TRUE);
			SetBoot(fs, bt);
			SetMBRProps();
			SetToGo();
			break;
		case IDC_BOOT:
			EnableAdvancedBootOptions(TRUE, TRUE);
			if (selection_default == DT_IMG)
				ToggleImage(!IsChecked(IDC_BOOT));
			break;
		case IDC_BOOTTYPE:
			if (HIWORD(wParam) != CBN_SELCHANGE)
				break;
			selection_default = (int) ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
			EnableAdvancedBootOptions(TRUE, TRUE);
			ToggleImage(!IsChecked(IDC_BOOT) || (selection_default != DT_IMG));
			SetToGo();
			if ((selection_default == DT_ISO) || (selection_default == DT_IMG)) {
				if ((image_path == NULL) || (iso_report.label[0] == 0)) {
					// Set focus to the Select ISO button
					SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)FALSE, 0);
					SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)hSelectISO, TRUE);
				} else {
					// Some distros (eg. Arch Linux) want to see a specific label => ignore user one
					SetWindowTextU(hLabel, iso_report.label);
				}
			} else {
				if (selection_default == DT_UEFI_NTFS) {
					// Try to select NTFS as default
					for (i=0; i<ComboBox_GetCount(hFileSystem); i++) {
						fs = (int)ComboBox_GetItemData(hFileSystem, i);
						if (fs == FS_NTFS)
							IGNORE_RETVAL(ComboBox_SetCurSel(hFileSystem, i));
					}
					SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE<<16) | IDC_FILESYSTEM,
						ComboBox_GetCurSel(hFileSystem));
				}
				// Set focus on the start button
				SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)FALSE, 0);
				SendMessage(hMainDialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hMainDialog, IDC_START), TRUE);
				// For non ISO, if the user manually set a label, try to preserve it
				if (!user_changed_label)
					SetWindowTextU(hLabel, SelectedDrive.proposed_label);
				// Reset disk ID to 0x80 if Rufus MBR is used
				IGNORE_RETVAL(ComboBox_SetCurSel(hDiskID, 0));
			}
			return (INT_PTR)TRUE;
		case IDC_SELECT_ISO:
			if (iso_provided) {
				uprintf("Image provided: '%s'\n", image_path);
				iso_provided = FALSE;	// One off thing...
			} else {
				safe_free(image_path);
				image_path = FileDialog(FALSE, NULL, (selection_default == DT_IMG)?&img_ext:&iso_ext, 0);
				if (image_path == NULL) {
					CreateTooltip(hSelectISO, lmprintf(MSG_173), -1);
					PrintStatus(0, MSG_086);
					break;
				}
			}
			selection_default = DT_ISO;
			CreateTooltip(hSelectISO, image_path, -1);
			FormatStatus = 0;
			if (CreateThread(NULL, 0, ISOScanThread, NULL, 0, NULL) == NULL) {
				uprintf("Unable to start ISO scanning thread");
				FormatStatus = ERROR_SEVERITY_ERROR|FAC(FACILITY_STORAGE)|APPERR(ERROR_CANT_START_THREAD);
			}
			break;
		case IDC_WINDOWS_INSTALL:
		case IDC_WINDOWS_TO_GO:
			if ( (Button_GetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_INSTALL)) == BST_CHECKED) ||
				 (Button_GetCheck(GetDlgItem(hMainDialog, IDC_WINDOWS_TO_GO)) == BST_CHECKED) ) {
				SetFSFromISO();
				SetMBRProps();
			}
			break;
		case IDC_RUFUS_MBR:
			if ((HIWORD(wParam)) == BN_CLICKED)
				mbr_selected_by_user = IsChecked(IDC_RUFUS_MBR);
			break;
		case IDC_ENABLE_FIXED_DISKS:
			if ((HIWORD(wParam)) == BN_CLICKED) {
				enable_HDDs = !enable_HDDs;
				PrintStatus2000(lmprintf(MSG_253), enable_HDDs);
				GetUSBDevices(0);
			}
			break;
		case IDC_START:
			if (format_thid != NULL) {
				return (INT_PTR)TRUE;
			}
			FormatStatus = 0;
			format_op_in_progress = TRUE;
			// Reset all progress bars
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
			SetTaskbarProgressState(TASKBAR_NORMAL);
			SetTaskbarProgressValue(0, MAX_PROGRESS);
			SendMessage(hProgress, PBM_SETPOS, 0, 0);
			selection_default = (int)ComboBox_GetItemData(hBootType, ComboBox_GetCurSel(hBootType));
			nDeviceIndex = ComboBox_GetCurSel(hDeviceList);
			if (nDeviceIndex != CB_ERR) {
				if ((IsChecked(IDC_BOOT)) && (!BootCheck())) {
					format_op_in_progress = FALSE;
					break;
				}

				// Display a warning about UDF formatting times
				fs = (int)ComboBox_GetItemData(hFileSystem, ComboBox_GetCurSel(hFileSystem));
				if (fs == FS_UDF) {
					dur_secs = (uint32_t)(((double)SelectedDrive.DiskSize)/1073741824.0f/UDF_FORMAT_SPEED);
					if (dur_secs > UDF_FORMAT_WARN) {
						dur_mins = dur_secs/60;
						dur_secs -= dur_mins*60;
						MessageBoxU(hMainDialog, lmprintf(MSG_112, dur_mins, dur_secs), lmprintf(MSG_113), MB_OK|MB_ICONASTERISK|MB_IS_RTL);
					} else {
						dur_secs = 0;
						dur_mins = 0;
					}
				}

				GetWindowTextU(hDeviceList, tmp, ARRAYSIZE(tmp));
				if (MessageBoxU(hMainDialog, lmprintf(MSG_003, tmp),
					APPLICATION_NAME, MB_OKCANCEL|MB_ICONWARNING|MB_IS_RTL) == IDCANCEL) {
					format_op_in_progress = FALSE;
					break;
				}
				if ((SelectedDrive.nPartitions > 1) && (MessageBoxU(hMainDialog, lmprintf(MSG_093),
					lmprintf(MSG_094), MB_OKCANCEL|MB_ICONWARNING|MB_IS_RTL) == IDCANCEL)) {
					format_op_in_progress = FALSE;
					break;
				}
				if ((IsChecked(IDC_BOOT)) && (SelectedDrive.Geometry.BytesPerSector != 512) && 
					(MessageBoxU(hMainDialog, lmprintf(MSG_196, SelectedDrive.Geometry.BytesPerSector),
						lmprintf(MSG_197), MB_OKCANCEL|MB_ICONWARNING|MB_IS_RTL) == IDCANCEL)) {
					format_op_in_progress = FALSE;
					break;
				}

				// Disable all controls except cancel
				EnableControls(FALSE);
				DeviceNum = (DWORD)ComboBox_GetItemData(hDeviceList, nDeviceIndex);
				FormatStatus = 0;
				InitProgress(FALSE);
				format_thid = CreateThread(NULL, 0, FormatThread, (LPVOID)(uintptr_t)DeviceNum, 0, NULL);
				if (format_thid == NULL) {
					uprintf("Unable to start formatting thread");
					FormatStatus = ERROR_SEVERITY_ERROR|FAC(FACILITY_STORAGE)|APPERR(ERROR_CANT_START_THREAD);
					PostMessage(hMainDialog, UM_FORMAT_COMPLETED, 0, 0);
				}
				uprintf("\r\nFormat operation started");
				PrintInfo(0, -1);
				timer = 0;
				safe_sprintf(szTimer, sizeof(szTimer), "00:00:00");
				SendMessageA(GetDlgItem(hMainDialog, IDC_STATUS), SB_SETTEXTA,
					SBT_OWNERDRAW | 1, (LPARAM)szTimer);
				SetTimer(hMainDialog, TID_APP_TIMER, 1000, ClockTimer);
			}
			if (format_thid == NULL)
				format_op_in_progress = FALSE;
			break;
		default:
			return (INT_PTR)FALSE;
		}
		return (INT_PTR)TRUE;

	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code) {
		case TBN_DROPDOWN:
			lpnmtb = (LPNMTOOLBAR)lParam;
			
			// We only care about the language button on the language toolbar
			if (lpnmtb->hdr.hwndFrom == hLangToolbar
				&& lpnmtb->iItem == lang_button_id) {
				// Get toolbar button rect and map it to actual screen pixels
				SendMessage(lpnmtb->hdr.hwndFrom, TB_GETRECT, (WPARAM)lpnmtb->iItem, (LPARAM)&LangToolbarRect);
				MapWindowPoints(lpnmtb->hdr.hwndFrom, NULL, (POINT*)&LangToolbarRect, 2);

				// Show the language menu such that it doesn't overlap the button
				ShowLanguageMenu(LangToolbarRect);
				return (INT_PTR)TBDDRET_DEFAULT;
			}
			break;
		}

		break;

	case WM_CLOSE:
		if (format_thid != NULL) {
			return (INT_PTR)TRUE;
		}
		PostQuitMessage(0);
		break;

	case UM_PROGRESS_INIT:
		isMarquee = (wParam == PBS_MARQUEE);
		if (isMarquee) {
			progress_style = GetWindowLong(hProgress, GWL_STYLE);
			SetWindowLong(hProgress, GWL_STYLE, progress_style | PBS_MARQUEE);
			SendMessage(hProgress, PBM_SETMARQUEE, TRUE, 0);
		} else {
			SendMessage(hProgress, PBM_SETPOS, 0, 0);
		}
		SetTaskbarProgressState(TASKBAR_NORMAL);
		SetTaskbarProgressValue(0, MAX_PROGRESS);
		
		break;

	case UM_PROGRESS_EXIT:
		if (isMarquee) {
			// Remove marquee style if previously set
			progress_style = GetWindowLong(hProgress, GWL_STYLE);
			SetWindowLong(hProgress, GWL_STYLE, progress_style & (~PBS_MARQUEE));
			SetTaskbarProgressValue(0, MAX_PROGRESS);
			SendMessage(hProgress, PBM_SETPOS, 0, 0);
		} else if (!IS_ERROR(FormatStatus)) {
			SetTaskbarProgressValue(MAX_PROGRESS, MAX_PROGRESS);
			// This is the only way to achieve instantaneous progress transition to 100%
			SendMessage(hProgress, PBM_SETRANGE, 0, ((MAX_PROGRESS+1)<<16) & 0xFFFF0000);
			SendMessage(hProgress, PBM_SETPOS, (MAX_PROGRESS+1), 0);
			SendMessage(hProgress, PBM_SETRANGE, 0, (MAX_PROGRESS<<16) & 0xFFFF0000);
		}
		SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);
		SetTaskbarProgressState(TASKBAR_NORMAL);
		break;

	case UM_NO_UPDATE:
		Notification(MSG_INFO, NULL, lmprintf(MSG_243), lmprintf(MSG_247));
		break;

	case UM_FORMAT_COMPLETED:
		format_thid = NULL;
		// Stop the timer
		KillTimer(hMainDialog, TID_APP_TIMER);
		// Close the cancel MessageBox and Blocking notification if active
		SendMessage(FindWindowA(MAKEINTRESOURCEA(32770), lmprintf(MSG_049)), WM_COMMAND, IDNO, 0);
		SendMessage(FindWindowA(MAKEINTRESOURCEA(32770), lmprintf(MSG_049)), WM_COMMAND, IDYES, 0);
		EnableWindow(GetDlgItem(hMainDialog, IDCANCEL), TRUE);
		EnableControls(TRUE);
		uprintf("\r\n");
		GetUSBDevices(DeviceNum);
		if (!IS_ERROR(FormatStatus)) {
			// This is the only way to achieve instantaneous progress transition to 100%
			SendMessage(hProgress, PBM_SETRANGE, 0, ((MAX_PROGRESS+1)<<16) & 0xFFFF0000);
			SendMessage(hProgress, PBM_SETPOS, (MAX_PROGRESS+1), 0);
			SendMessage(hProgress, PBM_SETRANGE, 0, (MAX_PROGRESS<<16) & 0xFFFF0000);
			SetTaskbarProgressState(TASKBAR_NOPROGRESS);
			PrintInfo(0, MSG_210);
		} else if (SCODE_CODE(FormatStatus) == ERROR_CANCELLED) {
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_PAUSED, 0);
			SetTaskbarProgressState(TASKBAR_PAUSED);
			PrintInfo(0, MSG_211);
			Notification(MSG_INFO, NULL, lmprintf(MSG_211), lmprintf(MSG_041));
		} else {
			SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
			SetTaskbarProgressState(TASKBAR_ERROR);
			PrintInfo(0, MSG_212);
			Notification(MSG_ERROR, NULL, lmprintf(MSG_042), lmprintf(MSG_043, StrError(FormatStatus, FALSE)));
		}
		FormatStatus = 0;
		format_op_in_progress = FALSE;
		return (INT_PTR)TRUE;
	}
	return (INT_PTR)FALSE;
}

static void PrintUsage(char* appname)
{
	char fname[_MAX_FNAME];

	_splitpath(appname, NULL, NULL, fname, NULL);
	printf("\nUsage: %s [-f] [-g] [-h] [-i PATH] [-l LOCALE] [-w TIMEOUT]\n", fname);
	printf("  -f, --fixed\n");
	printf("     Enable the listing of fixed/HDD USB drives\n");
	printf("  -g, --gui\n");
	printf("     Start in GUI mode (disable the 'rufus.com' commandline hogger)\n");
	printf("  -i PATH, --iso=PATH\n");
	printf("     Select the ISO image pointed by PATH to be used on startup\n");
	printf("  -l LOCALE, --locale=LOCALE\n");
	printf("     Select the locale to be used on startup\n");
	printf("  -w TIMEOUT, --wait=TIMEOUT\n");
	printf("     Wait TIMEOUT tens of seconds for the global application mutex to be released.\n");
	printf("     Used when launching a newer version of " APPLICATION_NAME " from a running application.\n");
	printf("  -h, --help\n");
	printf("     This usage guide.\n");
}

static HANDLE SetHogger(BOOL attached_console, BOOL disable_hogger)
{
	INPUT* input;
	BYTE* hog_data;
	DWORD hog_size, Size;
	HANDLE hogmutex = NULL, hFile = NULL;
	int i;

	if (!attached_console)
		return NULL;

	hog_data = GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_XT_HOGGER),
		_RT_RCDATA, cmdline_hogger, &hog_size, FALSE);
	if ((hog_data != NULL) && (!disable_hogger)) {
		// Create our synchronisation mutex
		hogmutex = CreateMutexA(NULL, TRUE, "Global/Rufus_CmdLine");

		// Extract the hogger resource
		hFile = CreateFileA(cmdline_hogger, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
			NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			// coverity[check_return]
			WriteFile(hFile, hog_data, hog_size, &Size, NULL);
		}
		safe_closehandle(hFile);

		// Now launch the file from the commandline, by simulating keypresses
		input = (INPUT*)calloc(strlen(cmdline_hogger)+1, sizeof(INPUT));
		for (i=0; i<(int)strlen(cmdline_hogger); i++) {
			input[i].type = INPUT_KEYBOARD;
			input[i].ki.dwFlags = KEYEVENTF_UNICODE;
			input[i].ki.wScan = (wchar_t)cmdline_hogger[i];
		}
		input[i].type = INPUT_KEYBOARD;
		input[i].ki.wVk = VK_RETURN;
		SendInput(i+1, input, sizeof(INPUT));
		safe_free(input);
	}
	if (hogmutex != NULL)
		Sleep(200);	// Need to add a delay, otherwise we may get some printout before the hogger
	return hogmutex;
}

/*
 * Application Entrypoint
 */
#if defined(_MSC_VER) && (_MSC_VER >= 1600)
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
#else
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#endif
{
	const char* rufus_loc = "rufus.loc";
	int i, opt, option_index = 0, argc = 0, si = 0, lcid = GetUserDefaultUILanguage();
	FILE* fd;
	BOOL attached_console = FALSE, external_loc_file = FALSE, lgp_set = FALSE, automount, disable_hogger = FALSE;
	BYTE *loc_data;
	DWORD loc_size, Size;
	char tmp_path[MAX_PATH] = "", loc_file[MAX_PATH] = "", ini_path[MAX_PATH], ini_flags[] = "rb";
	char *tmp, *locale_name = NULL, **argv = NULL;
	wchar_t **wenv, **wargv;
	PF_TYPE_DECL(CDECL, int,  __wgetmainargs, (int*, wchar_t***, wchar_t***, int, int*));
	HANDLE mutex = NULL, hogmutex = NULL, hFile = NULL;
	HWND hDlg = NULL;
	MSG msg;
	int wait_for_mutex = 0;
	struct option long_options[] = {
		{"fixed",   no_argument,       NULL, 'f'},
		{"gui",     no_argument,       NULL, 'g'},
		{"help",    no_argument,       NULL, 'h'},
		{"iso",     required_argument, NULL, 'i'},
		{"locale",  required_argument, NULL, 'l'},
		{"wait",    required_argument, NULL, 'w'},
		{0, 0, NULL, 0}
	};

	uprintf("*** " APPLICATION_NAME " init ***\n");

	// Reattach the console, if we were started from commandline
	if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
		attached_console = TRUE;
		IGNORE_RETVAL(freopen("CONIN$", "r", stdin));
		IGNORE_RETVAL(freopen("CONOUT$", "w", stdout));
		IGNORE_RETVAL(freopen("CONOUT$", "w", stderr));
		_flushall();
	}

	// We have to process the arguments before we acquire the lock and process the locale
	PF_INIT(__wgetmainargs, Msvcrt);
	if (pf__wgetmainargs != NULL) {
		pf__wgetmainargs(&argc, &wargv, &wenv, 1, &si);
		argv = (char**)calloc(argc, sizeof(char*));

		// Non getopt parameter check
		for (i=0; i<argc; i++) {
			argv[i] = wchar_to_utf8(wargv[i]);
			// Check for " /W" (wait for mutex release for pre 1.3.3 versions)
			if (strcmp(argv[i], "/W") == 0)
				wait_for_mutex = 150;	// Try to acquire the mutex for 15 seconds
			// We need to find if we need to disable the hogger BEFORE we start
			// processing arguments with getopt, as we may want to print messages
			// on the commandline then, which the hogger makes more intuitive.
			if ((strcmp(argv[i], "-g") == 0) || (strcmp(argv[i], "--gui") == 0))
				disable_hogger = TRUE;
		}

		// If our application name contains a 'p' (for "portable") create a 'rufus.ini'
		// NB: argv[0] is populated in the previous loop
		tmp = &argv[0][strlen(argv[0]) -1];
		while ((((uintptr_t)tmp)>((uintptr_t)argv[0])) && (*tmp != '\\'))
			tmp--;
		if (strchr(tmp, 'p') != NULL)
			ini_flags[0] = 'a';

		// Now enable the hogger before processing the rest of the arguments
		hogmutex = SetHogger(attached_console, disable_hogger);

		while ((opt = getopt_long(argc, argv, "?fghi:w:l:", long_options, &option_index)) != EOF)
			switch (opt) {
			case 'f':
				enable_HDDs = TRUE;
				break;
			case 'g':
				// No need to reprocess that option
				break;
			case 'i':
				if (_access(optarg, 0) != -1) {
					image_path = safe_strdup(optarg);
					iso_provided = TRUE;
				} else {
					printf("Could not find ISO image '%s'\n", optarg);
				}
				break;
			case 'l':
				if (isdigitU(optarg[0])) {
					lcid = (int)strtol(optarg, NULL, 0);
				} else {
					safe_free(locale_name);
					locale_name =safe_strdup(optarg);
				}
				break;
			case 'w':
				wait_for_mutex = atoi(optarg);
				break;
			case '?':
			case 'h':
			default:
				PrintUsage(argv[0]);
				goto out;
		}
	} else {
		uprintf("unable to access UTF-16 args");
	}

	// Retrieve the current application directory
	GetCurrentDirectoryU(MAX_PATH, app_dir);

	// Look for a .ini file in the current app directory
	static_sprintf(ini_path, "%s\\rufus.ini", app_dir);
	fd = fopenU(ini_path, ini_flags);	// Will create the file if portable mode is requested
	if (fd != NULL) {
		ini_file = ini_path;
		fclose(fd);
	}
	uprintf("Will use settings from %s", (ini_file != NULL)?"INI file":"registry");

	// Use the locale specified by the settings, if any
	tmp = ReadSettingStr(SETTING_LOCALE);
	if (tmp[0] != 0) {
		locale_name = safe_strdup(tmp);
		uprintf("found locale '%s'", locale_name);
	}

	// Init localization
	init_localization();
	// Seek for a loc file in the current directory
	if (GetFileAttributesU(rufus_loc) == INVALID_FILE_ATTRIBUTES) {
		uprintf("loc file not found in current directory - embedded one will be used");

		loc_data = (BYTE*)GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_LC_RUFUS_LOC), _RT_RCDATA, "embedded.loc", &loc_size, FALSE);
		if ( (GetTempPathU(sizeof(tmp_path), tmp_path) == 0)
		  || (GetTempFileNameU(tmp_path, APPLICATION_NAME, 0, loc_file) == 0)
		  || (loc_file[0] == 0) ) {
			// Last ditch effort to get a loc file - just extract it to the current directory
			safe_strcpy(loc_file, sizeof(loc_file), rufus_loc);
		}

		hFile = CreateFileU(loc_file, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
			NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if ((hFile == INVALID_HANDLE_VALUE) || (!WriteFile(hFile, loc_data, loc_size, &Size, 0)) || (loc_size != Size)) {
			uprintf("localization: unable to extract '%s': %s.\n", loc_file, WindowsErrorString());
			safe_closehandle(hFile);
			goto out;
		}
		uprintf("localization: extracted data to '%s'\n", loc_file);
		safe_closehandle(hFile);
	} else {
		safe_sprintf(loc_file, sizeof(loc_file), "%s\\%s", app_dir, rufus_loc);
		external_loc_file = TRUE;
		uprintf("using external loc file '%s'", loc_file);
	}

	if ( (!get_supported_locales(loc_file))
	  || ((selected_locale = ((locale_name == NULL)?get_locale_from_lcid(lcid, TRUE):get_locale_from_name(locale_name, TRUE))) == NULL) ) {
		uprintf("FATAL: Could not access locale!\n");
		MessageBoxU(NULL, "The locale data is missing or invalid. This application will now exit.",
			"Fatal error", MB_ICONSTOP|MB_IS_RTL|MB_SYSTEMMODAL);
		goto out;
	}

	// Prevent 2 applications from running at the same time, unless "/W" is passed as an option
	// in which case we wait for the mutex to be relinquished
	if ((safe_strlen(lpCmdLine)==2) && (lpCmdLine[0] == '/') && (lpCmdLine[1] == 'W'))
		wait_for_mutex = 150;		// Try to acquire the mutex for 15 seconds
	mutex = CreateMutexA(NULL, TRUE, "Global/" APPLICATION_NAME);
	for (;(wait_for_mutex>0) && (mutex != NULL) && (GetLastError() == ERROR_ALREADY_EXISTS); wait_for_mutex--) {
		CloseHandle(mutex);
		Sleep(100);
		mutex = CreateMutexA(NULL, TRUE, "Global/" APPLICATION_NAME);
	}
	if ((mutex == NULL) || (GetLastError() == ERROR_ALREADY_EXISTS)) {
		// Load the translation before we print the error
		get_loc_data_file(loc_file, selected_locale);
		// Set MB_SYSTEMMODAL to prevent Far Manager from stealing focus...
		MessageBoxU(NULL, lmprintf(MSG_002), lmprintf(MSG_001), MB_ICONSTOP|MB_IS_RTL|MB_SYSTEMMODAL);
		goto out;
	}

	// Save instance of the application for further reference
	hMainInstance = hInstance;

	// Initialize COM for folder selection
	IGNORE_RETVAL(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));

	// Some dialogs have Rich Edit controls and won't display without this
	if (GetLibraryHandle("Riched20") == NULL) {
		uprintf("Could not load RichEdit library - some dialogs may not display: %s\n", WindowsErrorString());
	}

	// Set the Windows version
	GetWindowsVersion();

	// We use local group policies rather than direct registry manipulation
	// 0x9e disables removable and fixed drive notifications
	lgp_set = SetLGP(FALSE, &existing_key, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", "NoDriveTypeAutorun", 0x9e);

	if (nWindowsVersion > WINDOWS_XP) {
		// Re-enable AutoMount if needed
		if (!GetAutoMount(&automount)) {
			uprintf("Could not get AutoMount status");
			automount = TRUE;	// So that we don't try to change its status on exit
		} else if (!automount) {
			uprintf("AutoMount was detected as disabled - temporary re-enabling it");
			if (!SetAutoMount(TRUE))
				uprintf("Failed to enable AutoMount");
		}
	}
	srand((unsigned int)GetTickCount());

relaunch:
	uprintf("localization: using locale '%s'\n", selected_locale->txt[0]);
	right_to_left_mode = ((selected_locale->ctrl_id) & LOC_RIGHT_TO_LEFT);
	SetProcessDefaultLayout(right_to_left_mode?LAYOUT_RTL:0);
	if (get_loc_data_file(loc_file, selected_locale))
		WriteSettingStr(SETTING_LOCALE, selected_locale->txt[0]);

	/*
	 * Create the main Window
	 *
	 * Oh yeah, thanks to Microsoft limitations for dialog boxes this is SUPER SUCKY:
	 * As per the MSDN [http://msdn.microsoft.com/en-ie/goglobal/bb688119.aspx], "The only way
	 *   to switch between mirrored and nonmirrored dialog resources at run time is to have two
	 *   sets of dialog resources: one mirrored and one nonmirrored."
	 * Unfortunately, this limitation is VERY REAL, so that's what we have to go through, and
	 * furthermore, trying to switch part of the dialogs back to LTR is also a major exercise
	 * in frustration, because it's next to impossible to figure out which combination of
	 * WS_EX_RTLREADING, WS_EX_RIGHT, WS_EX_LAYOUTRTL, WS_EX_LEFTSCROLLBAR and ES_RIGHT will
	 * work... and there's no way to toggle ES_RIGHT at runtime anyway.
	 * So, just like Microsoft advocates, we go through a massive duplication of all our RC
	 * dialogs (our RTL dialogs having their IDD's offset by +100 - see IDD_OFFSET), just to
	 * add a handful of stupid flags. And of course, we also have to go through a whole other
	 * exercise just so that our RTL and non RTL duplicated dialogs are kept in sync...
	 */
	hDlg = CreateDialogW(hInstance, MAKEINTRESOURCEW(IDD_DIALOG + IDD_OFFSET), NULL, MainCallback);
	if (hDlg == NULL) {
		MessageBoxU(NULL, "Could not create Window", "DialogBox failure", MB_ICONSTOP|MB_IS_RTL|MB_SYSTEMMODAL);
		goto out;
	}
	if ((relaunch_rc.left > -65536) && (relaunch_rc.top > -65536))
		SetWindowPos(hDlg, HWND_TOP, relaunch_rc.left, relaunch_rc.top, 0, 0, SWP_NOSIZE);
	ShowWindow(hDlg, SW_SHOWNORMAL);
	UpdateWindow(hDlg);

	// Do our own event processing and process "magic" commands
	while(GetMessage(&msg, NULL, 0, 0)) {

		// Ctrl-A => Select the log data
		if ( (IsWindowVisible(hLogDlg)) && (GetKeyState(VK_CONTROL) & 0x8000) && 
			(msg.message == WM_KEYDOWN) && (msg.wParam == 'A') ) {
			// Might also need ES_NOHIDESEL property if you want to select when not active
			SendMessage(hLog, EM_SETSEL, 0, -1);
		}
		// Alt-B => Toggle fake drive detection during bad blocks check
		// By default, Rufus will check for fake USB flash drives that mistakenly present
		// more capacity than they already have by looping over the flash. This check which
		// is enabled by default is performed by writing the block number sequence and reading
		// it back during the bad block check.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'B')) {
			detect_fakes = !detect_fakes;
			PrintStatus2000(lmprintf(MSG_256), detect_fakes);
			continue;
		}
		// Alt C => Force the update check to be successful
		// This will set the reported current version of Rufus to 0.0.0.0 when performing an update
		// check, so that it always succeeds. This is useful for translators.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'C')) {
			force_update = !force_update;
			PrintStatus2000(lmprintf(MSG_259), force_update);
			continue;
		}
		// Alt-D => Delete the NoDriveTypeAutorun key on exit (useful if the app crashed)
		// This key is used to disable Windows popup messages when an USB drive is plugged in.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'D')) {
			PrintStatus(2000, MSG_255);
			existing_key = FALSE;
			continue;
		}
		// Alt-E => Enhanced installation mode (allow dual UEFI/BIOS mode and FAT32 for Windows)
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'E')) {
			allow_dual_uefi_bios = !allow_dual_uefi_bios;
			PrintStatus2000(lmprintf(MSG_266), allow_dual_uefi_bios);
			continue;
		}
		// Alt-F => Toggle detection of USB HDDs
		// By default Rufus does not list USB HDDs. This is a safety feature aimed at avoiding
		// unintentional formatting of backup drives instead of USB keys.
		// When enabled, Rufus will list and allow the formatting of USB HDDs.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'F')) {
			enable_HDDs = !enable_HDDs;
			PrintStatus2000(lmprintf(MSG_253), enable_HDDs);
			GetUSBDevices(0);
			CheckDlgButton(hMainDialog, IDC_ENABLE_FIXED_DISKS, enable_HDDs?BST_CHECKED:BST_UNCHECKED);
			continue;
		}
		// Alt-I => Toggle ISO support
		// This is useful if you have a dual ISO/DD image and you want to force Rufus to use
		// DD-mode when writing the data.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'I')) {
			enable_iso = !enable_iso;
			PrintStatus2000(lmprintf(MSG_262), enable_iso);
			if (image_path != NULL) {
				iso_provided = TRUE;
				PostMessage(hDlg, WM_COMMAND, IDC_SELECT_ISO, 0);
			}
			continue;
		}
		// Alt J => Toggle Joliet support for ISO9660 images
		// Some ISOs (Ubuntu) have Joliet extensions but expect applications not to use them,
		// due to their reliance on filenames that are > 64 chars (the Joliet max length for
		// a file name). This option allows users to ignore Joliet when using such images.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'J')) {
			enable_joliet = !enable_joliet;
			PrintStatus2000(lmprintf(MSG_257), enable_joliet);
			continue;
		}
		// Alt K => Toggle Rock Ridge support for ISO9660 images
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'K')) {
			enable_rockridge = !enable_rockridge;
			PrintStatus2000(lmprintf(MSG_258), enable_rockridge);
			continue;
		}
		// Alt-L => Force Large FAT32 format to be used on < 32 GB drives
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'L')) {
			force_large_fat32 = !force_large_fat32;
			PrintStatus2000(lmprintf(MSG_254), force_large_fat32);
			GetUSBDevices(0);
			continue;
		}
		// Alt N => Enable NTFS compression
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'N')) {
			enable_ntfs_compression = !enable_ntfs_compression;
			PrintStatus2000(lmprintf(MSG_260), enable_ntfs_compression);
			continue;
		}
		// Alt-R => Remove all the registry keys that may have been created by Rufus
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'R')) {
			PrintStatus(2000, DeleteRegistryKey(REGKEY_HKCU, COMPANY_NAME "\\" APPLICATION_NAME)?MSG_248:MSG_249);
			// Also try to delete the upper key (company name) if it's empty (don't care about the result)
			DeleteRegistryKey(REGKEY_HKCU, COMPANY_NAME);
			continue;
		}
		// Alt-S => Disable size limit for ISOs
		// By default, Rufus will not copy ISOs that are larger than in size than
		// the target USB drive. If this is enabled, the size check is disabled.
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'S')) {
			size_check = !size_check;
			PrintStatus2000(lmprintf(MSG_252), size_check);
			GetUSBDevices(0);
			continue;
		}
		// Alt-U => Use PROPER size units, instead of this whole Kibi/Gibi nonsense
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'U')) {
			use_fake_units = !use_fake_units;
			PrintStatus2000(lmprintf(MSG_263), !use_fake_units);
			GetUSBDevices(0);
			continue;
		}
		// Alt-W => Enable VMWare disk detection
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'W')) {
			enable_vmdk = !enable_vmdk;
			PrintStatus2000(lmprintf(MSG_265), enable_vmdk);
			GetUSBDevices(0);
			continue;
		}
		// Alt-X => Delete the 'rufus_files' subdirectory
		if ((msg.message == WM_SYSKEYDOWN) && (msg.wParam == 'X')) {
			static_sprintf(tmp_path, "%s\\%s", app_dir, FILES_DIR);
			PrintStatus(2000, MSG_264, tmp_path);
			SHDeleteDirectoryExU(NULL, tmp_path, FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION);
			continue;
		}

		// Let the system handle dialog messages (e.g. those from the tab key)
		if (!IsDialogMessage(hDlg, &msg) && !IsDialogMessage(hLogDlg, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	if (relaunch) {
		relaunch = FALSE;
		reinit_localization();
		goto relaunch;
	}

out:
	// Destroy the hogger mutex first, so that the cmdline app can exit and we can delete it
	if (attached_console && !disable_hogger) {
		ReleaseMutex(hogmutex);
		safe_closehandle(hogmutex);
	}
	if ((!external_loc_file) && (loc_file[0] != 0))
		DeleteFileU(loc_file);
	DestroyAllTooltips();
	exit_localization();
	safe_free(image_path);
	safe_free(locale_name);
	safe_free(update.download_url);
	safe_free(update.release_notes);
	safe_free(grub2_buf);
	if (argv != NULL) {
		for (i=0; i<argc; i++) safe_free(argv[i]);
		safe_free(argv);
	}
	if (lgp_set)
		SetLGP(TRUE, &existing_key, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", "NoDriveTypeAutorun", 0);
	if ((nWindowsVersion > WINDOWS_XP) && (!automount) && (!SetAutoMount(FALSE)))
		uprintf("Failed to restore AutoMount to disabled");
	if (attached_console) {
		SetWindowPos(GetConsoleWindow(), HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
		FreeConsole();
	}
	// Unconditional delete, just in case...
	DeleteFileA(cmdline_hogger);
	CloseHandle(mutex);
	CLOSE_OPENED_LIBRARIES;
	uprintf("*** " APPLICATION_NAME " exit ***\n");
#ifdef _CRTDBG_MAP_ALLOC
	_CrtDumpMemoryLeaks();
#endif

	return 0;
}
