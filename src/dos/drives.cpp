/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include "dosbox.h"
#include "dos_system.h"
#include "bios_disk.h"
#include "drives.h"
#include "setup.h"
#include "mapper.h"
#include "support.h"
#include "control.h"
#include "ide.h"

char *DBCS_upcase(char *);

bool wildmount = false;

bool wild_match(const char *haystack, char *needle) {
	size_t max, i;
    for (; *needle != '\0'; ++needle) {
        switch (*needle) {
        case '?':
            if (*haystack == '\0')
                return false;
            ++haystack;
            break;
        case '*':
            if (needle[1] == '\0')
                return true;
            max = strlen(haystack);
            for (i = 0; i < max; i++)
                if (wild_match(haystack + i, needle + 1))
                    return true;
            return false;
        default:
            if (toupper(*haystack) != *needle)
                return false;
            ++haystack;
        }
    }
    return *haystack == '\0';
}

bool WildFileCmp(const char * file, const char * wild) 
{
	if (!file||!wild) return false;
	char file_name[9];
	char file_ext[4];
    char wild_name[10];
    char wild_ext[5];
	const char * find_ext;
	Bitu r;

    for (r=0;r<9;r++) {
        file_name[r]=0;
        wild_name[r]=0;
    }
    wild_name[9]=0;
    for (r=0;r<4;r++) {
        file_ext[r]=0;
        wild_ext[r]=0;
    }
    wild_ext[4]=0;

	find_ext=strrchr(file,'.');
	if (find_ext) {
		Bitu size=(Bitu)(find_ext-file);
		if (size>8) size=8;
		memcpy(file_name,file,size);
		find_ext++;
		memcpy(file_ext,find_ext,(strlen(find_ext)>3) ? 3 : strlen(find_ext)); 
	} else {
		memcpy(file_name,file,(strlen(file) > 8) ? 8 : strlen(file));
	}
	upcase(file_name);upcase(file_ext);
	find_ext=strrchr(wild,'.');
	if (find_ext) {
		Bitu size=(Bitu)(find_ext-wild);
		if (size>9) size=9;
		memcpy(wild_name,wild,size);
		find_ext++;
		memcpy(wild_ext,find_ext,(strlen(find_ext)>4) ? 4 : strlen(find_ext));
	} else {
		memcpy(wild_name,wild,(strlen(wild) > 9) ? 9 : strlen(wild));
	}
	upcase(wild_name);upcase(wild_ext);
	/* Names are right do some checking */
	if (uselfn&&strchr(wild_name, '*')) {
		if (strchr(wild,'.')) {
			if (!wild_match(file_name, wild_name)) return false;
			goto checkext;
		} else
			return wild_match(file, wild_name);
	} else {
		r=0;
		while (r<8) {
			if (wild_name[r]=='*') goto checkext;
			if (wild_name[r]!='?' && wild_name[r]!=file_name[r]) return false;
			r++;
		}
		if (wild_name[r]&&wild_name[r]!='*') return false;
	}
checkext:
	if (uselfn&&strchr(wild_ext, '*'))
		return wild_match(file_ext, wild_ext);
	else {
		r=0;
		while (r<3) {
			if (wild_ext[r]=='*') return true;
			if (wild_ext[r]!='?' && wild_ext[r]!=file_ext[r]) return false;
			r++;
		}
		if (wild_ext[r]&&wild_ext[r]!='*') return false;
		return true;
	}
}

bool LWildFileCmp(const char * file, const char * wild)
{
    if ((!uselfn&&!wildmount)||!file||!wild||(*file&&!*wild)||strlen(wild)>LFN_NAMELENGTH) return false;
    char file_name[256];
    char file_ext[256];
    char wild_name[256];
    char wild_ext[256];
    const char * find_ext;
    Bitu r;

    for (r=0;r<256;r++) {
      file_name[r]=0;
      wild_name[r]=0;
    }
    for (r=0;r<256;r++) {
      file_ext[r]=0;
      wild_ext[r]=0;
    }

    Bitu size,elen;
    find_ext=strrchr(file,'.');
    if (find_ext) {
            size=(Bitu)(find_ext-file);
            if (size>255) size=255;
            memcpy(file_name,file,size);
            find_ext++;
            elen=strlen(find_ext);
            memcpy(file_ext,find_ext,(strlen(find_ext)>255) ? 255 : strlen(find_ext));
    } else {
            size=strlen(file);
            elen=0;
            memcpy(file_name,file,(strlen(file) > 255) ? 255 : strlen(file));
    }
    upcase(file_name);upcase(file_ext);
    char nwild[LFN_NAMELENGTH+2];
    strcpy(nwild,wild);
    if (strrchr(nwild,'*')&&strrchr(nwild,'.')==NULL) strcat(nwild,".*");
    find_ext=strrchr(nwild,'.');
    if (find_ext) {
            if (wild_match(file, nwild)) return true;
            Bitu size=(Bitu)(find_ext-nwild);
            if (size>255) size=255;
            memcpy(wild_name,nwild,size);
            find_ext++;
            memcpy(wild_ext,find_ext,(strlen(find_ext)>255) ? 255 : strlen(find_ext));
    } else {
            memcpy(wild_name,nwild,(strlen(nwild) > 255) ? 255 : strlen(nwild));
    }
    upcase(wild_name);upcase(wild_ext);
    /* Names are right do some checking */
	if (strchr(wild_name, '*')) {
		if (strchr(wild,'.')) {
			if (!wild_match(file_name, wild_name)) return false;
			goto checkext;
		} else
			return wild_match(file, wild_name);
	} else {
		r=0;
		while (r<size) {
				if (wild_name[r]=='*') goto checkext;
				if (wild_name[r]!='?' && wild_name[r]!=file_name[r]) return false;
				r++;
		}
		if (wild_name[r]&&wild_name[r]!='*') return false;
	}
checkext:
	if (strchr(wild_ext, '*'))
		return wild_match(file_ext, wild_ext);
	else {
		r=0;
		while (r<elen) {
				if (wild_ext[r]=='*') return true;
				if (wild_ext[r]!='?' && wild_ext[r]!=file_ext[r]) return false;
				r++;
		}
		if (wild_ext[r]&&wild_ext[r]!='*') return false;
		return true;
	}
}

host_cnv_char_t *CodePageGuestToHost(const char *s);
char *CodePageHostToGuest(const host_cnv_char_t *s);
int get_expanded_files(const std::string &path, std::vector<std::string> &paths, bool readonly) {
    std::vector<std::string> files, names;
    if (!path.size()) return 0;
    char full[CROSS_LEN], pdir[DOS_PATHLENGTH], pattern[DOS_PATHLENGTH], *r;
    strcpy(full, path.c_str());
    r=strrchr_dbcs(full, CROSS_FILESPLIT);
    if (r!=NULL) {
        *r=0;
        strcpy(pdir, full);
        strcpy(pattern, r+1);
        *r=CROSS_FILESPLIT;
    } else {
        strcpy(pdir, "");
        strcpy(pattern, full);
    }
    if (!strchr(pattern, '*')&&!strchr(pattern, '?')) return 0;

#if defined(WIN32)
    HANDLE hFind;
    WIN32_FIND_DATA fd;
    WIN32_FIND_DATAW fdw;
    host_cnv_char_t *host_name = CodePageGuestToHost((std::string(pdir)+"\\*.*").c_str());
    if (host_name != NULL) hFind = FindFirstFileW(host_name, &fdw);
    else hFind = FindFirstFile((std::string(pdir)+"\\*.*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const char* temp_name = NULL;
            if (host_name != NULL) temp_name = CodePageHostToGuest(fdw.cFileName);
            if (!((host_name != NULL ? fdw.dwFileAttributes : fd.dwFileAttributes) & FILE_ATTRIBUTE_DIRECTORY)) {
                if (host_name == NULL)
                    names.emplace_back(fd.cFileName);
                else if (temp_name != NULL)
                    names.emplace_back(temp_name);
            }
        } while(host_name != NULL ? FindNextFileW(hFind, &fdw) : FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
#else
    std::string homedir(pdir);
    Cross::ResolveHomedir(homedir);
    strcpy(pdir, homedir.c_str());
    struct dirent *dir;
    host_cnv_char_t *host_name = CodePageGuestToHost(pdir);
    DIR *d = opendir(host_name != NULL ? host_name : pdir);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            host_cnv_char_t *temp_name = CodePageHostToGuest(dir->d_name);
#if defined(HAIKU)
            struct stat path_stat;
            stat(dir->d_name, &path_stat);
            bool is_regular_file = S_ISREG(path_stat.st_mode);
#else
            bool is_regular_file = (dir->d_type == DT_REG);
#endif
            if (is_regular_file)
                names.push_back(temp_name!=NULL?temp_name:dir->d_name);
        }
        closedir(d);
    }
#endif
	wildmount = true;
	for (std::string name: names) {
		if (LWildFileCmp(name.c_str(), pattern))
			files.push_back((readonly ? ":":"") + std::string(pdir) + std::string(1, CROSS_FILESPLIT) + name);
	}
	wildmount = false;

	if (files.size()) {
		sort(files.begin(), files.end());
		paths.insert(paths.end(), files.begin(), files.end());
		return files.size();
	} else {
		return 0;
	}
}

void Set_Label(char const * const input, char * const output, bool cdrom) {
    uint8_t togo = 11;
    uint8_t vnamePos = 0;
    uint8_t labelPos = 0;
    char upcasebuf[12];
    bool str_end = false; // True if end of string is detected
    strncpy(upcasebuf, input, 11);
    //DBCS_upcase(upcasebuf);  /* Another mscdex quirk. Label is not always uppercase. (Daggerfall) */ 

    while (togo > 0) {
        if(upcasebuf[vnamePos] == 0 && !str_end) {
            str_end = true;
            if(cdrom && vnamePos == 8) output[labelPos++] = '.';
            //Add a trailing dot ('.') when on cdrom and label is exactly 8 characters, MSCDEX feature/bug (fifa96 cdrom detection)
        }
        else if(cdrom && vnamePos == 8 && !str_end && upcasebuf[vnamePos] != '.') {
            output[labelPos] = '.'; // add a dot between 8th and 9th character (Descent 2 installer needs this)
            labelPos++;
        }
        output[labelPos] = !str_end ? upcasebuf[vnamePos] : 0x0; // Pad empty characters with 0x00
        labelPos++;
        vnamePos++;
        togo--;
    }
    output[labelPos] = 0;
    return;
}

DOS_Drive::DOS_Drive() {
    nocachedir=false;
    readonly=false;
	curdir[0]=0;
	info[0]=0;
}

const char * DOS_Drive::GetInfo(void) {
	return info;
}

// static members variables
extern int32_t swapPosition;
extern int swapInDisksSpecificDrive;
extern bool dos_kernel_disabled;
int DriveManager::currentDrive;
DriveManager::DriveInfo DriveManager::driveInfos[26];

void DriveManager::AppendDisk(int drive, DOS_Drive* disk) {
	driveInfos[drive].disks.push_back(disk);
}

void DriveManager::ChangeDisk(int drive, DOS_Drive* disk) {
    DriveInfo& driveInfo = driveInfos[drive];
    if (Drives[drive]==NULL||disk==NULL||!driveInfo.disks.size()) return;
    isoDrive *cdrom = dynamic_cast<isoDrive*>(Drives[drive]);
    signed char index=-1;
    bool slave=false;
    if (cdrom) IDE_CDROM_Detach_Ret(index,slave,drive);
    strcpy(disk->curdir,driveInfo.disks[driveInfo.currentDisk]->curdir);
    disk->Activate();
    if (!dos_kernel_disabled) disk->UpdateDPB(currentDrive);
    else if (cdrom) cdrom->loadImage();
    driveInfo.disks[driveInfo.currentDisk] = disk;
    fatDrive *old = dynamic_cast<fatDrive*>(Drives[drive]);
    Drives[drive] = disk;
    if (cdrom && index>-1) IDE_CDROM_Attach(index,slave,drive);
    Drives[drive]->EmptyCache();
    Drives[drive]->MediaChange();
    if (cdrom && !dos_kernel_disabled) {
        IDE_CDROM_Detach_Ret(index,slave,drive);
        if (index>-1) IDE_CDROM_Attach(index,slave,drive);
    }
    fatDrive *fdp = dynamic_cast<fatDrive*>(Drives[drive]);
    if (drive<2 && fdp && fdp->loadedDisk) {
        if (imageDiskList[drive]) {
            imageDiskList[drive]->Release();
            imageDiskList[drive] = fdp->loadedDisk;
            imageDiskList[drive]->Addref();
            imageDiskChange[drive] = true;
        }
        if (swapInDisksSpecificDrive == drive && diskSwap[swapPosition]) {
            diskSwap[swapPosition]->Release();
            diskSwap[swapPosition] = fdp->loadedDisk;
            diskSwap[swapPosition]->Addref();
        }
        if (!dos_kernel_disabled) {
            char name[DOS_NAMELENGTH_ASCII],lname[LFN_NAMELENGTH];
            uint32_t size,hsize;uint16_t date;uint16_t time;uint8_t attr;
            RealPt save_dta = dos.dta();
            dos.dta(dos.tables.tempdta);
            DOS_DTA dta(dos.dta());
            char root[7] = {(char)('A'+drive),':','\\','*','.','*',0};
            bool ret = DOS_FindFirst(root,DOS_ATTR_VOLUME);
            if (ret) {
                dta.GetResult(name,lname,size,hsize,date,time,attr);
                DOS_FindNext();
            } else name[0] = 0;
            dos.dta(save_dta);
        }
    }
    if (old) old->UnMount();
}

void DriveManager::ClearDrive(int drive) {
	UnmountDrive(drive); // which calls UnMount which drives delete themselves
	if (dos_kernel_disabled && !driveInfos[drive].disks.empty()) E_Exit("Drive Manager ClearDrive UnmountDrive failed to clear disk swap chain");
	Drives[drive] = NULL;
}

void DriveManager::InitializeDrive(int drive) {
	currentDrive = drive;
	DriveInfo& driveInfo = driveInfos[currentDrive];
	if (driveInfo.disks.size() > 0) {
		driveInfo.currentDisk = 0;
		DOS_Drive* disk = driveInfo.disks[driveInfo.currentDisk];
		Drives[currentDrive] = disk;
		if (driveInfo.disks.size() > 1) disk->Activate();
		disk->UpdateDPB(currentDrive);
	}
}

/*
void DriveManager::CycleDrive(bool pressed) {
	if (!pressed) return;
		
	// do one round through all drives or stop at the next drive with multiple disks
	int oldDrive = currentDrive;
	do {
		currentDrive = (currentDrive + 1) % DOS_DRIVES;
		int numDisks = driveInfos[currentDrive].disks.size();
		if (numDisks > 1) break;
	} while (currentDrive != oldDrive);
}

void DriveManager::CycleDisk(bool pressed) {
	if (!pressed) return;
	
	int numDisks = driveInfos[currentDrive].disks.size();
	if (numDisks > 1) {
		// cycle disk
		int currentDisk = driveInfos[currentDrive].currentDisk;
		DOS_Drive* oldDisk = driveInfos[currentDrive].disks[currentDisk];
		currentDisk = (currentDisk + 1) % numDisks;		
		DOS_Drive* newDisk = driveInfos[currentDrive].disks[currentDisk];
		driveInfos[currentDrive].currentDisk = currentDisk;
		
		// copy working directory, acquire system resources and finally switch to next drive		
		strcpy(newDisk->curdir, oldDisk->curdir);
		newDisk->Activate();
		Drives[currentDrive] = newDisk;
	}
}
*/

void DriveManager::CycleDisks(int drive, bool notify, bool rev, unsigned int position) {
	unsigned int numDisks = (unsigned int)driveInfos[drive].disks.size();
	if (numDisks > 1) {
		// cycle disk
		unsigned int currentDisk = driveInfos[drive].currentDisk;
		const DOS_Drive* oldDisk = driveInfos[drive].disks[currentDisk];
        if(position < 1) {
            if(rev)
                currentDisk = (currentDisk == 0) ? numDisks - 1 : currentDisk - 1;
            else
                currentDisk = (currentDisk + 1u) % numDisks;
        }
        else if(position > numDisks)
			currentDisk = 0;
		else
			currentDisk = position - 1;
		DOS_Drive* newDisk = driveInfos[drive].disks[currentDisk];
		driveInfos[drive].currentDisk = currentDisk;
		if (drive < MAX_DISK_IMAGES && imageDiskList[drive] != NULL) {
			imageDiskList[drive]->Release();
			imageDiskList[drive] = NULL;

			if (strncmp(newDisk->GetInfo(),"fatDrive",8) == 0)
				imageDiskList[drive] = ((fatDrive *)newDisk)->loadedDisk;
			else
				imageDiskList[drive] = (imageDisk *)newDisk;

			if (imageDiskList[drive] != NULL) imageDiskList[drive]->Addref();
			if ((drive == 2 || drive == 3) && imageDiskList[drive]->hardDrive) updateDPT();
		}

		// copy working directory, acquire system resources and finally switch to next drive
		strcpy(newDisk->curdir, oldDisk->curdir);
		newDisk->Activate();
		if (!dos_kernel_disabled) newDisk->UpdateDPB(drive);
		Drives[drive] = newDisk;
		if (notify) LOG_MSG("Drive %c: disk %d of %d now active", 'A'+drive, currentDisk+1, numDisks);
	}
}

void DriveManager::CycleAllDisks(bool rev) {
	for (int idrive=0; idrive<2; idrive++) CycleDisks(idrive, true, rev); /* Cycle all DISKS meaning A: and B: */
}

void DriveManager::CycleAllCDs(bool rev) {
	for (unsigned int idrive=2; idrive<DOS_DRIVES; idrive++) { /* Cycle all CDs in C: D: ... Z: */
		unsigned int numDisks = (unsigned int)driveInfos[idrive].disks.size();
		if (numDisks > 1) {
			// cycle disk
			unsigned int currentDisk = driveInfos[idrive].currentDisk;
			const DOS_Drive* oldDisk = driveInfos[idrive].disks[currentDisk];
			if (dynamic_cast<const isoDrive*>(oldDisk) == NULL) continue;
            if(rev)
                currentDisk = (currentDisk == 0) ? numDisks - 1 : currentDisk - 1;
            else
                currentDisk = (currentDisk + 1u) % numDisks;
			DOS_Drive* newDisk = driveInfos[idrive].disks[currentDisk];
			driveInfos[idrive].currentDisk = currentDisk;

			// copy working directory, acquire system resources and finally switch to next drive
			strcpy(newDisk->curdir, oldDisk->curdir);
			newDisk->Activate();
			if (!dos_kernel_disabled) newDisk->UpdateDPB(currentDrive);
			Drives[idrive] = newDisk;
			LOG_MSG("Drive %c: disk %d of %d now active", 'A'+idrive, currentDisk+1, numDisks);
		}
	}
}

int DriveManager::UnmountDrive(int drive) {
	int result = 0;
	// unmanaged drive
	if (driveInfos[drive].disks.size() == 0) {
		if (!dos_kernel_disabled) result = (int)Drives[drive]->UnMount();
		else delete Drives[drive];
		driveInfos[drive].currentDisk = 0;
	} else {
		// managed drive
		unsigned int currentDisk = driveInfos[drive].currentDisk;
		if (!dos_kernel_disabled) result = (int)driveInfos[drive].disks[currentDisk]->UnMount();
		else delete driveInfos[drive].disks[currentDisk];
		// only delete on success, current disk set to NULL because of UnMount
		if (result == 0) {
			driveInfos[drive].disks[currentDisk] = NULL;
			for (unsigned int i = 0; i < (unsigned int)driveInfos[drive].disks.size(); i++) {
				delete driveInfos[drive].disks[i];
			}
			driveInfos[drive].disks.clear();
			driveInfos[drive].currentDisk = 0;
		}
	}
	
	return result;
}

int DriveManager::GetDisksSize(int drive) {
    return (int)driveInfos[drive].disks.size();
}

char swappos[10];
char * DriveManager::GetDrivePosition(int drive) {
    sprintf(swappos, "%d / %d", driveInfos[drive].currentDisk+1, (int)driveInfos[drive].disks.size());
    return swappos;
}

bool drivemanager_init = false;
bool int13_extensions_enable = true;
bool int13_disk_change_detect_enable = true;
bool int13_enable_48bitLBA = true;

void DriveManager::Init(Section* s) {
    const Section_prop* section = static_cast<Section_prop*>(s);

	drivemanager_init = true;

	int13_extensions_enable = section->Get_bool("int 13 extensions");
	int13_disk_change_detect_enable = section->Get_bool("int 13 disk change detect");
    int13_enable_48bitLBA = section->Get_bool("int 13 enable 48-bit LBA");

	// setup driveInfos structure
	currentDrive = 0;
	for(int i = 0; i < DOS_DRIVES; i++) {
		driveInfos[i].currentDisk = 0;
	}
	
//	MAPPER_AddHandler(&CycleDisk, MK_f3, MMOD1, "cycledisk", "Cycle Disk");
//	MAPPER_AddHandler(&CycleDrive, MK_f3, MMOD2, "cycledrive", "Cycle Drv");
}

void DRIVES_Startup(Section *s) {
    (void)s;//UNUSED
	if (!drivemanager_init) {
		LOG(LOG_DOSMISC,LOG_DEBUG)("Initializing drive system");
		DriveManager::Init(control->GetSection("dos"));
	}
}

void DRIVES_Init() {
	LOG(LOG_DOSMISC,LOG_DEBUG)("Initializing DOS drives");

	// TODO: DOS kernel exit, reset, guest booting handler
}

char * DOS_Drive::GetBaseDir(void) {
	return info + 16;
}

// save state support
void DOS_Drive::SaveState( std::ostream& stream )
{
	// - pure data
	WRITE_POD( &nocachedir, nocachedir );
	WRITE_POD( &readonly, readonly );
	WRITE_POD( &curdir, curdir );
	WRITE_POD( &info, info );
}

void DOS_Drive::LoadState( std::istream& stream )
{
	// - pure data
	READ_POD( &nocachedir, nocachedir );
	READ_POD( &readonly, readonly );
	READ_POD( &curdir, curdir );
	READ_POD( &info, info );
}

void DriveManager::SaveState( std::ostream& stream )
{
	// - pure data
	WRITE_POD( &currentDrive, currentDrive );
}

void DriveManager::LoadState( std::istream& stream )
{
	// - pure data
	READ_POD( &currentDrive, currentDrive );
}

void POD_Save_DOS_DriveManager( std::ostream& stream )
{
	DriveManager::SaveState(stream);
}

void POD_Load_DOS_DriveManager( std::istream& stream )
{
	DriveManager::LoadState(stream);
}

#if defined(C_KIOSK)
bool MSCDEX_HasDrive(char driveLetter);

static std::string Kiosk_StripWriteProtectPrefix(const std::string& path) {
	if (path.size() > 1 && path[0] == ':' && path[1] != '\\' && path[1] != '/') {
		return path.substr(1);
	}
	return path;
}

static std::string Kiosk_ExtractDriveHostPath(DOS_Drive* drv, int drive) {
	if (drv != NULL) {
		fatDrive* fdp = dynamic_cast<fatDrive*>(drv);
		if (fdp != NULL && fdp->loadedDisk != NULL && !fdp->loadedDisk->diskname.empty()) {
			return Kiosk_StripWriteProtectPrefix(fdp->loadedDisk->diskname);
		}
		const char* info = drv->GetInfo();
		if (info != NULL && *info != '\0') {
			if (strncmp(info, "isoDrive ", 9) == 0) {
				return Kiosk_StripWriteProtectPrefix(std::string(info + 9));
			}
			if (strncmp(info, "fatDrive ", 9) == 0) {
				return Kiosk_StripWriteProtectPrefix(std::string(info + 9));
			}
			if (strncmp(info, "local directory ", 16) == 0) {
				return std::string(info + 16);
			}
		}
	}
	if (drive >= 0 && drive < MAX_DISK_IMAGES && imageDiskList[drive] != NULL && !imageDiskList[drive]->diskname.empty()) {
		return Kiosk_StripWriteProtectPrefix(imageDiskList[drive]->diskname);
	}
	return std::string();
}

int DriveManager::GetKioskSlotCount(int drive) {
	if (drive < 0 || drive >= DOS_DRIVES) return 0;
	if (!driveInfos[drive].disks.empty()) {
		return (int)driveInfos[drive].disks.size();
	}
	if (drive < 2 && swapInDisksSpecificDrive == drive) {
		int count = 0;
		for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
			if (diskSwap[i] != NULL) ++count;
		}
		if (count > 0) return count;
	}
	if (drive < MAX_DISK_IMAGES && imageDiskList[drive] != NULL) return 1;
	if (Drives[drive] != NULL) return 1;
	return 0;
}

int DriveManager::GetKioskCurrentSlot(int drive) {
	if (drive < 0 || drive >= DOS_DRIVES) return 0;
	if (!driveInfos[drive].disks.empty()) {
		return (int)driveInfos[drive].currentDisk;
	}
	if (drive < 2 && swapInDisksSpecificDrive == drive) {
		if (swapPosition >= 0 && swapPosition < MAX_SWAPPABLE_DISKS) {
			return (int)swapPosition;
		}
	}
	return 0;
}

static bool kiosk_drive_ejected[DOS_DRIVES] = {};

void Kiosk_DOS_SyncEjectedCDROM(unsigned char drive_index) {
	if (drive_index >= DOS_DRIVES) return;
	if (Drives[drive_index] != NULL) {
		isoDrive* iso = dynamic_cast<isoDrive*>(Drives[drive_index]);
		if (iso != NULL) {
			iso->KioskSyncCDROMRef();
		}
		Drives[drive_index]->EmptyCache();
	}
}

std::string DriveManager::GetKioskActiveDiskPath(int drive) {
	if (drive < 0 || drive >= DOS_DRIVES) return std::string();
	if (kiosk_drive_ejected[drive]) return std::string();
	if (drive < 2 && imageDiskList[drive] != NULL && !imageDiskList[drive]->diskname.empty()) {
		return Kiosk_StripWriteProtectPrefix(imageDiskList[drive]->diskname);
	}
	if (Drives[drive] != NULL) {
		std::string p = Kiosk_ExtractDriveHostPath(Drives[drive], drive);
		if (!p.empty()) return p;
	}
	if (!driveInfos[drive].disks.empty() && driveInfos[drive].currentDisk < driveInfos[drive].disks.size()) {
		return Kiosk_ExtractDriveHostPath(driveInfos[drive].disks[driveInfos[drive].currentDisk], drive);
	}
	return std::string();
}

std::vector<std::string> DriveManager::GetKioskDrivePaths(int drive) {
	std::vector<std::string> paths;
	if (drive < 0 || drive >= DOS_DRIVES) return paths;

	if (!driveInfos[drive].disks.empty()) {
		for (size_t i = 0; i < driveInfos[drive].disks.size(); ++i) {
			std::string p = Kiosk_ExtractDriveHostPath(driveInfos[drive].disks[i], -1);
			if (!p.empty()) paths.push_back(p);
		}
		return paths;
	}

	if (drive < 2 && swapInDisksSpecificDrive == drive) {
		for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
			if (diskSwap[i] != NULL && !diskSwap[i]->diskname.empty()) {
				paths.push_back(Kiosk_StripWriteProtectPrefix(diskSwap[i]->diskname));
			}
		}
	}

	if (paths.empty()) {
		std::string active = GetKioskActiveDiskPath(drive);
		if (!active.empty()) paths.push_back(active);
	}
	return paths;
}

static bool Kiosk_PathsEqual(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		char ca = (a[i] == '\\') ? '/' : a[i];
		char cb = (b[i] == '\\') ? '/' : b[i];
#if defined(WIN32)
		ca = (char)tolower((unsigned char)ca);
		cb = (char)tolower((unsigned char)cb);
#endif
		if (ca != cb) return false;
	}
	return true;
}

bool DriveManager::AppendAndSwapDisk(int drive, const std::string& host_path, bool is_cdrom, bool write_protected) {
	if (drive < 0 || drive >= DOS_DRIVES || host_path.empty()) return false;

	const std::string clean_path = Kiosk_StripWriteProtectPrefix(host_path);

	if (!is_cdrom) {
		if (drive >= 2) return false;

		bool effective_wp = write_protected;

		// Check already-mounted DriveManager slots BEFORE any fopen() call:
		// on Windows, IMGMOUNT opens floppy images via fopen_lock() with dwShareMode=0
		// (exclusive lock), so re-opening an already-mounted image with fopen() fails.
		bool mounted_via_fat = false;
		if (!dos_kernel_disabled && (!driveInfos[drive].disks.empty() || dos.tables.mediaid != 0)) {
			int existing_idx = -1;
			for (size_t i = 0; i < driveInfos[drive].disks.size(); ++i) {
				if (Kiosk_PathsEqual(Kiosk_ExtractDriveHostPath(driveInfos[drive].disks[i], -1), clean_path)) {
					existing_idx = (int)i;
					break;
				}
			}

			if (existing_idx >= 0) {
				if ((int)driveInfos[drive].currentDisk != existing_idx && driveInfos[drive].disks.size() > 1) {
					CycleDisks(drive, true, false, (unsigned int)(existing_idx + 1));
				} else {
					driveInfos[drive].currentDisk = (uint32_t)existing_idx;
					Drives[drive] = driveInfos[drive].disks[existing_idx];
				}
				if (Drives[drive] != NULL) {
					Drives[drive]->readonly = effective_wp;
					Drives[drive]->Activate();
					Drives[drive]->UpdateDPB(drive);
					Drives[drive]->EmptyCache();
					Drives[drive]->MediaChange();
					fatDrive* fdp = dynamic_cast<fatDrive*>(Drives[drive]);
					if (fdp != NULL && fdp->loadedDisk != NULL && imageDiskList[drive] != fdp->loadedDisk) {
						if (imageDiskList[drive] != NULL) imageDiskList[drive]->Release();
						imageDiskList[drive] = fdp->loadedDisk;
						imageDiskList[drive]->Addref();
					}
				}
				mounted_via_fat = true;
			} else {
				std::vector<std::string> opts;
				if (effective_wp) {
					opts.push_back("readonly");
				}
				fatDrive* new_fd = new fatDrive(clean_path.c_str(), 0, 0, 0, 0, opts);
				if (new_fd != NULL && new_fd->created_successfully && new_fd->loadedDisk != NULL) {
					if (new_fd->readonly) {
						effective_wp = true;
					} else {
						new_fd->readonly = effective_wp;
					}
					new_fd->loadedDisk->kiosk_write_protected = effective_wp;
					const bool was_empty = driveInfos[drive].disks.empty();
					AppendDisk(drive, new_fd);
					if (was_empty) {
						InitializeDrive(drive);
					} else {
						unsigned int new_pos = (unsigned int)driveInfos[drive].disks.size();
						CycleDisks(drive, true, false, new_pos);
					}
					Drives[drive] = new_fd;
					if (imageDiskList[drive] != new_fd->loadedDisk) {
						if (imageDiskList[drive] != NULL) imageDiskList[drive]->Release();
						imageDiskList[drive] = new_fd->loadedDisk;
						imageDiskList[drive]->Addref();
					}
					Drives[drive]->Activate();
					Drives[drive]->UpdateDPB(drive);
					Drives[drive]->EmptyCache();
					Drives[drive]->MediaChange();
					mounted_via_fat = true;
				} else {
					delete new_fd;
				}
			}
		}

		if (!mounted_via_fat) {
			int existing_swap = -1;
			if (swapInDisksSpecificDrive == drive) {
				for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
					if (diskSwap[i] != NULL && Kiosk_PathsEqual(Kiosk_StripWriteProtectPrefix(diskSwap[i]->diskname), clean_path)) {
						existing_swap = i;
						break;
					}
				}
			}

			if (existing_swap >= 0) {
				swapPosition = existing_swap;
				if (imageDiskList[drive] != NULL) imageDiskList[drive]->Release();
				imageDiskList[drive] = diskSwap[existing_swap];
				imageDiskList[drive]->Addref();
			} else {
				FILE* fp = fopen(clean_path.c_str(), effective_wp ? "rb" : "rb+");
				if (!fp && !effective_wp) {
					fp = fopen(clean_path.c_str(), "rb");
					if (fp) {
						effective_wp = true;
					}
				}
				if (!fp) {
					LOG_MSG("KIOSK DriveManager: Cannot open raw floppy image '%s'", clean_path.c_str());
					return false;
				}
				fseek(fp, 0, SEEK_END);
				long fsz = ftell(fp);
				fseek(fp, 0, SEEK_SET);
				if (fsz <= 0) {
					fclose(fp);
					return false;
				}
				imageDisk* new_disk = new imageDisk(fp, clean_path.c_str(), (uint64_t)fsz, false);
				new_disk->drvnum = (unsigned int)drive;
				new_disk->kiosk_write_protected = effective_wp;

				if (imageDiskList[drive] != NULL) imageDiskList[drive]->Release();
				imageDiskList[drive] = new_disk;
				imageDiskList[drive]->Addref();

				if (swapInDisksSpecificDrive == -1 || swapInDisksSpecificDrive == drive) {
					swapInDisksSpecificDrive = drive;
					for (int i = 0; i < MAX_SWAPPABLE_DISKS; ++i) {
						if (diskSwap[i] == NULL) {
							diskSwap[i] = new_disk;
							new_disk->Addref();
							swapPosition = i;
							break;
						}
					}
				}
			}
		}

		if (imageDiskList[drive] != NULL) {
			imageDiskList[drive]->kiosk_write_protected = effective_wp;
		}
		kiosk_drive_ejected[drive] = false;
		updateFloppyDPT();
		FDC_AssignINT13Disk((unsigned char)drive);
		FDC_NotifyDiskSwap(drive, 100.0, effective_wp);
		return true;
	}

	// Optical CD-ROM hot-swap (D:, E:, etc.)
	FILE* cd_fp = fopen(clean_path.c_str(), "rb");
	if (!cd_fp) {
		LOG_MSG("KIOSK DriveManager: Cannot open CD-ROM image '%s'", clean_path.c_str());
		return false;
	}
	fclose(cd_fp);

	int existing_cd_idx = -1;
	for (size_t i = 0; i < driveInfos[drive].disks.size(); ++i) {
		if (Kiosk_ExtractDriveHostPath(driveInfos[drive].disks[i], -1) == clean_path) {
			existing_cd_idx = (int)i;
			break;
		}
	}

	if (existing_cd_idx >= 0) {
		DOS_Drive* old_disk = Drives[drive];
		DOS_Drive* new_disk = driveInfos[drive].disks[existing_cd_idx];
		driveInfos[drive].currentDisk = (uint32_t)existing_cd_idx;
		if (old_disk != NULL && new_disk != old_disk) {
			strcpy(new_disk->curdir, old_disk->curdir);
		}
		new_disk->Activate();
		if (!dos_kernel_disabled) new_disk->UpdateDPB(drive);
		Drives[drive] = new_disk;
		Drives[drive]->EmptyCache();
		kiosk_drive_ejected[drive] = false;
		Kiosk_IDE_NotifyATAPIMediaChange((unsigned char)drive, 250.0);
		return true;
	}

	int error = -1;
	std::vector<std::string> options;
	const bool had_mscdex = MSCDEX_HasDrive((char)('A' + drive));
	if (dos_kernel_disabled && !had_mscdex) {
		// REAL_HDD_BOOT without a boot-time CD-ROM in MSCDEX: mount directly onto ATAPI IDE
		const bool ok = Kiosk_IDE_MountATAPIMedia((unsigned char)drive, clean_path.c_str(), 250.0);
		if (ok) kiosk_drive_ejected[drive] = false;
		return ok;
	}

	isoDrive* new_cd = new isoDrive((char)('A' + drive), clean_path.c_str(), 0xF8, error, options);
	if (error != 0) {
		delete new_cd;
		if (dos_kernel_disabled) {
			const bool ok = Kiosk_IDE_MountATAPIMedia((unsigned char)drive, clean_path.c_str(), 250.0);
			if (ok) kiosk_drive_ejected[drive] = false;
			return ok;
		}
		return false;
	}

	DOS_Drive* prev_drive = Drives[drive];
	const uint32_t prev_slot = driveInfos[drive].currentDisk;

	AppendDisk(drive, new_cd);
	driveInfos[drive].currentDisk = (uint32_t)(driveInfos[drive].disks.size() - 1);
	if (prev_drive != NULL) {
		strcpy(new_cd->curdir, prev_drive->curdir);
	}
	Drives[drive] = new_cd;
	new_cd->Activate();
	if (!dos_kernel_disabled) new_cd->UpdateDPB(drive);
	new_cd->EmptyCache();

	if (!had_mscdex) {
		signed char ide_idx = -1;
		bool slave = false;
		IDE_Auto(ide_idx, slave);
		if (ide_idx >= 0) {
			IDE_CDROM_Attach(ide_idx, slave, (unsigned char)drive);
		}
	}

	if (!Kiosk_IDE_MountATAPIMedia((unsigned char)drive, clean_path.c_str(), 250.0)) {
		driveInfos[drive].disks.pop_back();
		driveInfos[drive].currentDisk = prev_slot;
		Drives[drive] = prev_drive;
		delete new_cd;
		if (prev_drive != NULL) {
			prev_drive->Activate();
			if (!dos_kernel_disabled) prev_drive->UpdateDPB(drive);
			prev_drive->EmptyCache();
		}
		return false;
	}

	kiosk_drive_ejected[drive] = false;
	return true;
}

bool DriveManager::EjectDriveMedia(int drive, bool is_cdrom) {
	if (drive < 0 || drive >= DOS_DRIVES) return false;
	if (!is_cdrom) {
		if (drive >= 2) return false;
		if (imageDiskList[drive] != NULL) {
			imageDiskList[drive]->Release();
			imageDiskList[drive] = NULL;
		}
		if (!dos_kernel_disabled && Drives[drive] != NULL) {
			Drives[drive]->EmptyCache();
			Drives[drive] = NULL;
		}
		kiosk_drive_ejected[drive] = true;
		FDC_NotifyDiskEject(drive);
		return true;
	}
	const bool ok = Kiosk_IDE_EjectATAPIMedia((unsigned char)drive);
	if (ok) {
		kiosk_drive_ejected[drive] = true;
	}
	return ok;
}
#endif
