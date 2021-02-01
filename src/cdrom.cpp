/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *   schultz.ryan@gmail.com, http://rschultz.ath.cx/code.php               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Steet, Fifth Floor, Boston, MA 02111-1307 USA.            *
 ***************************************************************************/

/* 
* Handles all CD-ROM registers and functions.
*/

// This file has been updated to use newer code of PCSX Reloaded/Rearmed.
// New bug fixes/updates come with the code, and also things like CD
// lid interrupt for better CD swapping support.
//
// Also included is a fix for stuttering XA audio on slower devices, see
// comment/changes in cdrReadInterrupt()
//
// See https://github.com/notaz/pcsx_rearmed/blob/master/libpcsxcore/cdrom.c
// for the source of updates to this code.
// Credit goes to Notaz / PCSX Rearmed, senquack

#include "cdrom.h"
#include "cdriso.h"
#include "plugin_lib.h"
#include "ppf.h"
#include "psxdma.h"
#include "psxevents.h"

#if defined(CDR_LOG) || defined(CDR_LOG_I) || defined(CDR_LOG_IO)
static const char *CmdName[0x100]= {
    "CdlSync",     "CdlNop",       "CdlSetloc",  "CdlPlay",
    "CdlForward",  "CdlBackward",  "CdlReadN",   "CdlStandby",
    "CdlStop",     "CdlPause",     "CdlInit",    "CdlMute",
    "CdlDemute",   "CdlSetfilter", "CdlSetmode", "CdlGetmode",
    "CdlGetlocL",  "CdlGetlocP",   "CdlReadT",   "CdlGetTN",
    "CdlGetTD",    "CdlSeekL",     "CdlSeekP",   "CdlSetclock",
    "CdlGetclock", "CdlTest",      "CdlID",      "CdlReadS",
    "CdlReset",    NULL,           "CDlReadToc", NULL
};
#endif

#ifndef CDR_LOG
#define CDR_LOG(...)
#endif

#ifndef CDR_LOG_I
#define CDR_LOG_I(...)
#endif

#ifndef CDR_LOG_IO
#define CDR_LOG_IO(...)
#endif

#define SysPrintf printf

cdrStruct cdr;
static unsigned char *pTransfer;

/* CD-ROM magic numbers */
#define CdlSync        0
#define CdlNop         1
#define CdlSetloc      2
#define CdlPlay        3
#define CdlForward     4
#define CdlBackward    5
#define CdlReadN       6
#define CdlStandby     7
#define CdlStop        8
#define CdlPause       9
#define CdlInit        10
#define CdlMute        11
#define CdlDemute      12
#define CdlSetfilter   13
#define CdlSetmode     14
#define CdlGetmode     15
#define CdlGetlocL     16
#define CdlGetlocP     17
#define CdlReadT       18
#define CdlGetTN       19
#define CdlGetTD       20
#define CdlSeekL       21
#define CdlSeekP       22
#define CdlSetclock    23
#define CdlGetclock    24
#define CdlTest        25
#define CdlID          26
#define CdlReadS       27
#define CdlReset       28
#define CdlGetQ        29
#define CdlReadToc     30

unsigned char Test04[] = { 0 };
unsigned char Test05[] = { 0 };
unsigned char Test20[] = { 0x98, 0x06, 0x10, 0xC3 };
unsigned char Test22[] = { 0x66, 0x6F, 0x72, 0x20, 0x45, 0x75, 0x72, 0x6F };
unsigned char Test23[] = { 0x43, 0x58, 0x44, 0x32, 0x39 ,0x34, 0x30, 0x51 };

// cdr.Stat:
#define NoIntr		0
#define DataReady	1
#define Complete	2
#define Acknowledge	3
#define DataEnd		4
#define DiskError	5

/* Modes flags */
#define MODE_SPEED       (1<<7) // 0x80
#define MODE_STRSND      (1<<6) // 0x40 ADPCM on/off
#define MODE_SIZE_2340   (1<<5) // 0x20
#define MODE_SIZE_2328   (1<<4) // 0x10
#define MODE_SIZE_2048   (0<<4) // 0x00
#define MODE_SF          (1<<3) // 0x08 channel on/off
#define MODE_REPORT      (1<<2) // 0x04
#define MODE_AUTOPAUSE   (1<<1) // 0x02
#define MODE_CDDA        (1<<0) // 0x01

/* Status flags */
#define STATUS_PLAY      (1<<7) // 0x80
#define STATUS_SEEK      (1<<6) // 0x40
#define STATUS_READ      (1<<5) // 0x20
#define STATUS_SHELLOPEN (1<<4) // 0x10
#define STATUS_UNKNOWN3  (1<<3) // 0x08
#define STATUS_UNKNOWN2  (1<<2) // 0x04
#define STATUS_ROTATING  (1<<1) // 0x02
#define STATUS_ERROR     (1<<0) // 0x01

/* Errors */
#define ERROR_NOTREADY   (1<<7) // 0x80
#define ERROR_INVALIDCMD (1<<6) // 0x40
#define ERROR_INVALIDARG (1<<5) // 0x20

#define cdReadTime (PSXCLK / 75)

enum drive_state {
	DRIVESTATE_STANDBY = 0,
	DRIVESTATE_LID_OPEN,
	DRIVESTATE_RESCAN_CD,
	DRIVESTATE_PREPARE_CD,
	DRIVESTATE_STOPPED,
};

// for cdr.Seeked
enum seeked_state {
	SEEK_PENDING = 0,
	SEEK_DONE = 1,
};

static struct CdrStat stat;

static unsigned int msf2sec(const u8 *msf) {
	return ((msf[0] * 60 + msf[1]) * 75) + msf[2];
}

// for that weird psemu API..
static unsigned int fsm2sec(const u8 *msf) {
	return ((msf[2] * 60 + msf[1]) * 75) + msf[0];
}

static void sec2msf(unsigned int s, u8 *msf) {
	msf[0] = s / 75 / 60;
	s = s - msf[0] * 75 * 60;
	msf[1] = s / 75;
	s = s - msf[1] * 75;
	msf[2] = s;
}

// lookup table for crc calculation
static unsigned short crctab[256] = {
	0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108,
	0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF, 0x1231, 0x0210,
	0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B,
	0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE, 0x2462, 0x3443, 0x0420, 0x1401,
	0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE,
	0xF5CF, 0xC5AC, 0xD58D, 0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6,
	0x5695, 0x46B4, 0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D,
	0xC7BC, 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
	0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B, 0x5AF5,
	0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC,
	0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A, 0x6CA6, 0x7C87, 0x4CE4,
	0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD,
	0xAD2A, 0xBD0B, 0x8D68, 0x9D49, 0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13,
	0x2E32, 0x1E51, 0x0E70, 0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A,
	0x9F59, 0x8F78, 0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E,
	0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
	0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1,
	0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256, 0xB5EA, 0xA5CB,
	0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D, 0x34E2, 0x24C3, 0x14A0,
	0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xA7DB, 0xB7FA, 0x8799, 0x97B8,
	0xE75F, 0xF77E, 0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657,
	0x7676, 0x4615, 0x5634, 0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9,
	0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882,
	0x28A3, 0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
	0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92, 0xFD2E,
	0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07,
	0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1, 0xEF1F, 0xFF3E, 0xCF5D,
	0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74,
	0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

u16 calcCrc(const u8 *d, const int len) {
	u16 crc = 0;
	int i;

	for (i = 0; i < len; i++) {
		crc = crctab[(crc >> 8) ^ d[i]] ^ (crc << 8);
	}

	return ~crc;
}

// cdrInterrupt
#define CDR_INT(eCycle) { \
	psxEvqueueAdd(PSXINT_CDR, eCycle); \
}

// cdrReadInterrupt
#define CDREAD_INT(eCycle) { \
	psxEvqueueAdd(PSXINT_CDREAD, eCycle); \
}

// Next two interrupt macros are new from PCSX Reloaded/Rearmed.
// cdrLidSeekInterrupt
#define CDRLID_INT(eCycle) { \
	psxEvqueueAdd(PSXINT_CDRLID, eCycle); \
}

// cdrPlayInterrupt
#define CDRMISC_INT(eCycle) { \
	psxEvqueueAdd(PSXINT_CDRPLAY, eCycle); \
}

#define StopReading() { \
	if (cdr.Reading) { \
		cdr.Reading = 0; \
		psxEvqueueRemove(PSXINT_CDREAD); \
	} \
	cdr.StatP &= ~(STATUS_READ|STATUS_SEEK);\
}

#define StopCdda() { \
	if (cdr.Play) { \
		if (!Config.Cdda) CDR_stop(); \
		cdr.StatP &= ~STATUS_PLAY; \
		cdr.Play = FALSE; \
		cdr.FastForward = 0; \
		cdr.FastBackward = 0; \
	} \
}

#define SetResultSize(size) { \
	cdr.ResultP = 0; \
	cdr.ResultC = size; \
	cdr.ResultReady = 1; \
}

static void setIrq(void)
{
	if (cdr.Stat & cdr.Reg2)
		psxHu32ref(0x1070) |= SWAP32((u32)0x4);

	// When IRQ status bit gets set, ensure psxBranchTest() gets called as soon
	//  as possible, so HW IRQ exception gets handled
	ResetIoCycle();
}

// timing used in this function was taken from tests on real hardware
// (yes it's slow, but you probably don't want to modify it)
void cdrLidSeekInterrupt()
{
	switch (cdr.DriveState) {
	default:
	case DRIVESTATE_STANDBY:
		cdr.StatP &= ~STATUS_SEEK;

		if (CDR_getStatus(&stat) == -1)
			return;

		if (stat.Status & STATUS_SHELLOPEN)
		{
			StopCdda();
			cdr.DriveState = DRIVESTATE_LID_OPEN;
			CDRLID_INT(0x800);
		}
		break;

	case DRIVESTATE_LID_OPEN:
		if (CDR_getStatus(&stat) == -1)
			stat.Status &= ~STATUS_SHELLOPEN;

		// 02, 12, 10
		if (!(cdr.StatP & STATUS_SHELLOPEN)) {
			StopReading();
			cdr.StatP |= STATUS_SHELLOPEN;

			// could generate error irq here, but real hardware
			// only sometimes does that
			// (not done when lots of commands are sent?)

			CDRLID_INT(cdReadTime * 30);
			break;
		}
		else if (cdr.StatP & STATUS_ROTATING) {
			cdr.StatP &= ~STATUS_ROTATING;
		}
		else if (!(stat.Status & STATUS_SHELLOPEN)) {
			// closed now
			CheckCdrom();

			/* Check whether this is *not* a multi-cd image */
			if (cdrIsoMultidiskCount == 1) {
				/* If we are using per-disk memory cards,
				 * reload them */
				if (Config.McdSlot1 == 0) {
					update_memcards(1);
				}
				if (Config.McdSlot2 == 0) {
					update_memcards(2);
				}
				/* If config overrides are enabled, update
				 * CdromName */
				if (config_override_enabled) {
					const char *iso_file = GetIsoFile();
					if (iso_file && (*iso_file != '\0')) {
						set_cdrom_name(iso_file);
					}
				}
			}

			// cdr.StatP STATUS_SHELLOPEN is "sticky"
			// and is only cleared by CdlNop

			cdr.DriveState = DRIVESTATE_RESCAN_CD;
			CDRLID_INT(cdReadTime * 105);
			break;
		}

		// recheck for close
		CDRLID_INT(cdReadTime * 3);
		break;

	case DRIVESTATE_RESCAN_CD:
		cdr.StatP |= STATUS_ROTATING;
		cdr.DriveState = DRIVESTATE_PREPARE_CD;

		// this is very long on real hardware, over 6 seconds
		// make it a bit faster here...
		CDRLID_INT(cdReadTime * 150);
		break;

	case DRIVESTATE_PREPARE_CD:
		cdr.StatP |= STATUS_SEEK;

		cdr.DriveState = DRIVESTATE_STANDBY;
		CDRLID_INT(cdReadTime * 26);
		break;
	}
}

static void Find_CurTrack(const u8 *time)
{
	int current, sect;

	current = msf2sec(time);

	for (cdr.CurTrack = 1; cdr.CurTrack < cdr.ResultTN[1]; cdr.CurTrack++) {
		CDR_getTD(cdr.CurTrack + 1, cdr.ResultTD);
		sect = fsm2sec(cdr.ResultTD);
		if (sect - current >= 150)
			break;
	}
}

static void generate_subq(const u8 *time)
{
	unsigned char start[3], next[3];
	unsigned int this_s, start_s, next_s, pregap;
	int relative_s;

	CDR_getTD(cdr.CurTrack, start);
	if (cdr.CurTrack + 1 <= cdr.ResultTN[1]) {
		pregap = 150;
		CDR_getTD(cdr.CurTrack + 1, next);
	}
	else {
		// last track - cd size
		pregap = 0;
		next[0] = cdr.SetSectorEnd[2];
		next[1] = cdr.SetSectorEnd[1];
		next[2] = cdr.SetSectorEnd[0];
	}

	this_s = msf2sec(time);
	start_s = fsm2sec(start);
	next_s = fsm2sec(next);

	cdr.TrackChanged = FALSE;

	if (next_s - this_s < pregap) {
		cdr.TrackChanged = TRUE;
		cdr.CurTrack++;
		start_s = next_s;
	}

	cdr.subq.Index = 1;

	relative_s = this_s - start_s;
	if (relative_s < 0) {
		cdr.subq.Index = 0;
		relative_s = -relative_s;
	}
	sec2msf(relative_s, cdr.subq.Relative);

	cdr.subq.Track = itob(cdr.CurTrack);
	cdr.subq.Relative[0] = itob(cdr.subq.Relative[0]);
	cdr.subq.Relative[1] = itob(cdr.subq.Relative[1]);
	cdr.subq.Relative[2] = itob(cdr.subq.Relative[2]);
	cdr.subq.Absolute[0] = itob(time[0]);
	cdr.subq.Absolute[1] = itob(time[1]);
	cdr.subq.Absolute[2] = itob(time[2]);
}

static void ReadTrack(const u8 *time)
{
	unsigned char tmp[3];
	struct SubQ *subq;
	u16 crc;

	tmp[0] = itob(time[0]);
	tmp[1] = itob(time[1]);
	tmp[2] = itob(time[2]);

	if (memcmp(cdr.Prev, tmp, 3) == 0)
		return;

	CDR_LOG("ReadTrack *** %02x:%02x:%02x\n", tmp[0], tmp[1], tmp[2]);

	cdr.RErr = CDR_readTrack(tmp);
	memcpy(cdr.Prev, tmp, 3);

	if (CheckSBI(time))
		return;

	subq = (struct SubQ *)CDR_getBufferSub();
	if (subq != NULL && cdr.CurTrack == 1) {
		crc = calcCrc((u8 *)subq + 12, 10);
		if (crc == (((u16)subq->CRC[0] << 8) | subq->CRC[1])) {
			cdr.subq.Track = subq->TrackNumber;
			cdr.subq.Index = subq->IndexNumber;
			memcpy(cdr.subq.Relative, subq->TrackRelativeAddress, 3);
			memcpy(cdr.subq.Absolute, subq->AbsoluteAddress, 3);
		}
		else {
			CDR_LOG_I("subq bad crc @%02x:%02x:%02x\n",
				tmp[0], tmp[1], tmp[2]);
		}
	}
	else {
		generate_subq(time);
	}

	CDR_LOG(" -> %02x,%02x %02x:%02x:%02x %02x:%02x:%02x\n",
		cdr.subq.Track, cdr.subq.Index,
		cdr.subq.Relative[0], cdr.subq.Relative[1], cdr.subq.Relative[2],
		cdr.subq.Absolute[0], cdr.subq.Absolute[1], cdr.subq.Absolute[2]);
}

static void AddIrqQueue(unsigned short irq, unsigned long ecycle)
{
	if (cdr.Irq != 0) {
		if (irq == cdr.Irq || irq + 0x100 == cdr.Irq) {
			cdr.IrqRepeated = 1;
			CDR_INT(ecycle);
			return;
		}

		CDR_LOG_I("cdr: override cmd %02x -> %02x\n", cdr.Irq, irq);
	}

	cdr.Irq = irq;
	cdr.eCycle = ecycle;

	CDR_INT(ecycle);
}

static void cdrPlayInterrupt_Autopause()
{
	if ((cdr.Mode & MODE_AUTOPAUSE) && cdr.TrackChanged) {
		CDR_LOG( "CDDA STOP\n" );

		// Magic the Gathering
		// - looping territory cdda

		// ...?
		//cdr.ResultReady = 1;
		//cdr.Stat = DataReady;
		cdr.Stat = DataEnd;
		setIrq();

		StopCdda();
	}
	else if (cdr.Mode & MODE_REPORT) {

		cdr.Result[0] = cdr.StatP;
		cdr.Result[1] = cdr.subq.Track;
		cdr.Result[2] = cdr.subq.Index;

		if (cdr.subq.Absolute[2] & 0x10) {
			cdr.Result[3] = cdr.subq.Relative[0];
			cdr.Result[4] = cdr.subq.Relative[1] | 0x80;
			cdr.Result[5] = cdr.subq.Relative[2];
		}
		else {
			cdr.Result[3] = cdr.subq.Absolute[0];
			cdr.Result[4] = cdr.subq.Absolute[1];
			cdr.Result[5] = cdr.subq.Absolute[2];
		}

		cdr.Result[6] = 0;
		cdr.Result[7] = 0;

		// Rayman: Logo freeze (resultready + dataready)
		cdr.ResultReady = 1;
		cdr.Stat = DataReady;

		SetResultSize(8);
		setIrq();
	}
}

// also handles seek
void cdrPlayInterrupt()
{
	if (cdr.Seeked == SEEK_PENDING) {
		if (cdr.Stat) {
			CDR_LOG_I("cdrom: seek stat hack\n");
			CDRMISC_INT(0x1000);
			return;
		}
		SetResultSize(1);
		cdr.StatP |= STATUS_ROTATING;
		cdr.StatP &= ~STATUS_SEEK;
		cdr.Result[0] = cdr.StatP;
		cdr.Seeked = SEEK_DONE;
		if (cdr.Irq == 0) {
			cdr.Stat = Complete;
			setIrq();
		}

		if (cdr.SetlocPending) {
			memcpy(cdr.SetSectorPlay, cdr.SetSector, 4);
			cdr.SetlocPending = 0;
		}
		Find_CurTrack(cdr.SetSectorPlay);
		ReadTrack(cdr.SetSectorPlay);
		cdr.TrackChanged = FALSE;
	}

	if (!cdr.Play) return;

	CDR_LOG( "CDDA - %d:%d:%d\n",
		cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2] );

	if (memcmp(cdr.SetSectorPlay, cdr.SetSectorEnd, 3) == 0) {
		StopCdda();
		cdr.TrackChanged = TRUE;
	}

	if (!cdr.Irq && !cdr.Stat && (cdr.Mode & (MODE_AUTOPAUSE|MODE_REPORT)))
		cdrPlayInterrupt_Autopause();

	if (!cdr.Play) return;

	cdr.SetSectorPlay[2]++;
	if (cdr.SetSectorPlay[2] == 75) {
		cdr.SetSectorPlay[2] = 0;
		cdr.SetSectorPlay[1]++;
		if (cdr.SetSectorPlay[1] == 60) {
			cdr.SetSectorPlay[1] = 0;
			cdr.SetSectorPlay[0]++;
		}
	}

	CDRMISC_INT(cdReadTime);

	// update for CdlGetlocP/autopause
	generate_subq(cdr.SetSectorPlay);
}

void cdrInterrupt()
{
	u16 Irq = cdr.Irq;
	int no_busy_error = 0;
	int start_rotating = 0;
	int error = 0;
	int delay;
	unsigned int seekTime = 0;

	// Reschedule IRQ
	if (cdr.Stat) {
		CDR_LOG_I("cdrom: stat hack: %02x %x\n", cdr.Irq, cdr.Stat);
		CDR_INT(0x1000);
		return;
	}

	cdr.Ctrl &= ~0x80;

	// default response
	SetResultSize(1);
	cdr.Result[0] = cdr.StatP;
	cdr.Stat = Acknowledge;

	if (cdr.IrqRepeated) {
		cdr.IrqRepeated = 0;
		if (cdr.eCycle > psxRegs.cycle) {
			CDR_INT(cdr.eCycle);
			goto finish;
		}
	}

	cdr.Irq = 0;

	switch (Irq) {
		case CdlSync:
			// TOOD: sometimes/always return error?
			break;

		case CdlNop:
			if (cdr.DriveState != DRIVESTATE_LID_OPEN)
				cdr.StatP &= ~STATUS_SHELLOPEN;
			no_busy_error = 1;
			break;

		case CdlSetloc:
			break;

		do_CdlPlay:
		case CdlPlay:
			StopCdda();
			if (cdr.Seeked == SEEK_PENDING) {
				// XXX: wrong, should seek instead..
				cdr.Seeked = SEEK_DONE;
			}
			if (cdr.SetlocPending) {
				memcpy(cdr.SetSectorPlay, cdr.SetSector, 4);
				cdr.SetlocPending = 0;
			}

			// BIOS CD Player
			// - Pause player, hit Track 01/02/../xx (Setloc issued!!)

			if (cdr.ParamC == 0 || cdr.Param[0] == 0) {
				CDR_LOG("PLAY Resume @ %d:%d:%d\n",
					cdr.SetSectorPlay[0], cdr.SetSectorPlay[1], cdr.SetSectorPlay[2]);
			}
			else
			{
				int track = btoi( cdr.Param[0] );

				if (track <= cdr.ResultTN[1])
					cdr.CurTrack = track;

				CDR_LOG("PLAY track %d\n", cdr.CurTrack);

				if (CDR_getTD((u8)cdr.CurTrack, cdr.ResultTD) != -1) {
					cdr.SetSectorPlay[0] = cdr.ResultTD[2];
					cdr.SetSectorPlay[1] = cdr.ResultTD[1];
					cdr.SetSectorPlay[2] = cdr.ResultTD[0];
				}
			}

			/*
			Rayman: detect track changes
			- fixes logo freeze

			Twisted Metal 2: skip PREGAP + starting accurate SubQ
			- plays tracks without retry play

			Wild 9: skip PREGAP + starting accurate SubQ
			- plays tracks without retry play
			*/
			Find_CurTrack(cdr.SetSectorPlay);
			ReadTrack(cdr.SetSectorPlay);
			cdr.TrackChanged = FALSE;

			if (!Config.Cdda)
				CDR_play(cdr.SetSectorPlay);

			// Vib Ribbon: gameplay checks flag
			cdr.StatP &= ~STATUS_SEEK;
			cdr.Result[0] = cdr.StatP;

			cdr.StatP |= STATUS_PLAY;
			
			// BIOS player - set flag again
			cdr.Play = TRUE;

			CDRMISC_INT( cdReadTime );
			start_rotating = 1;
			break;

		case CdlForward:
			// TODO: error 80 if stopped
			cdr.Stat = Complete;

			// GameShark CD Player: Calls 2x + Play 2x
			if( cdr.FastForward == 0 ) cdr.FastForward = 2;
			else cdr.FastForward++;

			cdr.FastBackward = 0;
			break;

		case CdlBackward:
			cdr.Stat = Complete;

			// GameShark CD Player: Calls 2x + Play 2x
			if( cdr.FastBackward == 0 ) cdr.FastBackward = 2;
			else cdr.FastBackward++;

			cdr.FastForward = 0;
			break;

		case CdlStandby:
			if (cdr.DriveState != DRIVESTATE_STOPPED) {
				error = ERROR_INVALIDARG;
				goto set_error;
			}
			AddIrqQueue(CdlStandby + 0x100, cdReadTime * 125 / 2);
			start_rotating = 1;
			break;

		case CdlStandby + 0x100:
			cdr.Stat = Complete;
			break;

		case CdlStop:
			if (cdr.Play) {
				// grab time for current track
				CDR_getTD((u8)(cdr.CurTrack), cdr.ResultTD);

				cdr.SetSectorPlay[0] = cdr.ResultTD[2];
				cdr.SetSectorPlay[1] = cdr.ResultTD[1];
				cdr.SetSectorPlay[2] = cdr.ResultTD[0];
			}

			StopCdda();
			StopReading();

			delay = 0x800;
			if (cdr.DriveState == DRIVESTATE_STANDBY)
				delay = cdReadTime * 30 / 2;

			cdr.DriveState = DRIVESTATE_STOPPED;
			AddIrqQueue(CdlStop + 0x100, delay);
			break;

		case CdlStop + 0x100:
			cdr.StatP &= ~STATUS_ROTATING;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlPause:
			/*
			Gundam Battle Assault 2: much slower (*)
			- Fixes boot, gameplay

			Hokuto no Ken 2: slower
			- Fixes intro + subtitles

			InuYasha - Feudal Fairy Tale: slower
			- Fixes battles
			*/
			AddIrqQueue(CdlPause + 0x100, cdReadTime * 3);
			cdr.Ctrl |= 0x80;
			break;

		case CdlPause + 0x100:
			cdr.StatP &= ~STATUS_READ;
			cdr.Result[0] = cdr.StatP;
			cdr.Stat = Complete;
			break;

		case CdlInit:
			AddIrqQueue(CdlInit + 0x100, cdReadTime * 6);
			no_busy_error = 1;
			start_rotating = 1;
			break;

		case CdlInit + 0x100:
			cdr.Stat = Complete;
			break;

		case CdlMute:
			cdr.Muted = TRUE;
			break;

		case CdlDemute:
			cdr.Muted = FALSE;
			break;

		case CdlSetfilter:
			cdr.File = cdr.Param[0];
			cdr.Channel = cdr.Param[1];
			break;

		case CdlSetmode:
			no_busy_error = 1;
			break;

		case CdlGetmode:
			SetResultSize(6);
			cdr.Result[1] = cdr.Mode;
			cdr.Result[2] = cdr.File;
			cdr.Result[3] = cdr.Channel;
			cdr.Result[4] = 0;
			cdr.Result[5] = 0;
			no_busy_error = 1;
			break;

		case CdlGetlocL:
			SetResultSize(8);
			memcpy(cdr.Result, cdr.Transfer, 8);
			break;

		case CdlGetlocP:
			SetResultSize(8);
			memcpy(&cdr.Result, &cdr.subq, 8);

			if (!cdr.Play && !cdr.Reading)
				cdr.Result[1] = 0; // HACK?
			break;

		case CdlReadT: // SetSession?
			// really long
			AddIrqQueue(CdlReadT + 0x100, cdReadTime * 290 / 4);
			start_rotating = 1;
			break;

		case CdlReadT + 0x100:
			cdr.Stat = Complete;
			break;

		case CdlGetTN:
			SetResultSize(3);
			if (CDR_getTN(cdr.ResultTN) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0] |= STATUS_ERROR;
			} else {
				cdr.Stat = Acknowledge;
				cdr.Result[1] = itob(cdr.ResultTN[0]);
				cdr.Result[2] = itob(cdr.ResultTN[1]);
			}
			break;

		case CdlGetTD:
			cdr.Track = btoi(cdr.Param[0]);
			SetResultSize(4);
			if (CDR_getTD(cdr.Track, cdr.ResultTD) == -1) {
				cdr.Stat = DiskError;
				cdr.Result[0] |= STATUS_ERROR;
			} else {
				cdr.Stat = Acknowledge;
				cdr.Result[0] = cdr.StatP;
				cdr.Result[1] = itob(cdr.ResultTD[2]);
				cdr.Result[2] = itob(cdr.ResultTD[1]);
				cdr.Result[3] = itob(cdr.ResultTD[0]);
			}
			break;

		case CdlSeekL:
		case CdlSeekP:
			StopCdda();
			StopReading();
			cdr.StatP |= STATUS_SEEK;

			/*
			Crusaders of Might and Magic = 0.5x-4x
			- fix cutscene speech start

			Eggs of Steel = 2x-?
			- fix new game

			Medievil = ?-4x
			- fix cutscene speech

			Rockman X5 = 0.5-4x
			- fix capcom logo
			*/
			CDRMISC_INT(cdr.Seeked == SEEK_DONE ? 0x800 : cdReadTime * 4);
			cdr.Seeked = SEEK_PENDING;
			start_rotating = 1;
			break;

		case CdlTest:
			switch (cdr.Param[0]) {
				case 0x20: // System Controller ROM Version
					SetResultSize(4);
					memcpy(cdr.Result, Test20, 4);
					break;
				case 0x22:
					SetResultSize(8);
					memcpy(cdr.Result, Test22, 4);
					break;
				case 0x23: case 0x24:
					SetResultSize(8);
					memcpy(cdr.Result, Test23, 4);
					break;
			}
			no_busy_error = 1;
			break;

		case CdlID:
			AddIrqQueue(CdlID + 0x100, 20480);
			break;

		case CdlID + 0x100:
			SetResultSize(8);
			cdr.Result[0] = cdr.StatP;
			cdr.Result[1] = 0;
			cdr.Result[2] = 0;
			cdr.Result[3] = 0;

			// 0x10 - audio | 0x40 - disk missing | 0x80 - unlicensed
			if (CDR_getStatus(&stat) == -1 || stat.Type == 0 || stat.Type == 0xff) {
				cdr.Result[1] = 0xc0;
			}
			else {
				if (stat.Type == 2)
					cdr.Result[1] |= 0x10;
				if (CdromId[0] == '\0')
					cdr.Result[1] |= 0x80;
			}
			cdr.Result[0] |= (cdr.Result[1] >> 4) & 0x08;

			strncpy((char *)&cdr.Result[4], "PCSX", 4);
			cdr.Stat = Complete;
			break;

		case CdlReset:
			// yes, it really sets STATUS_SHELLOPEN
			cdr.StatP |= STATUS_SHELLOPEN;
			cdr.DriveState = DRIVESTATE_RESCAN_CD;
			CDRLID_INT(20480);
			no_busy_error = 1;
			start_rotating = 1;
			break;

		case CdlGetQ:
			// TODO?
			CDR_LOG_I("got CdlGetQ\n");
			break;

		case CdlReadToc:
			AddIrqQueue(CdlReadToc + 0x100, cdReadTime * 180 / 4);
			no_busy_error = 1;
			start_rotating = 1;
			break;

		case CdlReadToc + 0x100:
			cdr.Stat = Complete;
			no_busy_error = 1;
			break;

		case CdlReadN:
		case CdlReadS:
			if (cdr.SetlocPending) {
				seekTime = abs((int)msf2sec(cdr.SetSectorPlay) - (int)msf2sec(cdr.SetSector)) * (cdReadTime / 200);
				if(seekTime > 1000000) seekTime = 1000000;
				memcpy(cdr.SetSectorPlay, cdr.SetSector, 4);
				cdr.SetlocPending = 0;
			}
			Find_CurTrack(cdr.SetSectorPlay);

			if ((cdr.Mode & MODE_CDDA) && cdr.CurTrack > 1)
				// Read* acts as play for cdda tracks in cdda mode
				goto do_CdlPlay;

			cdr.Reading = 1;
			cdr.FirstSector = 1;

			// Fighting Force 2 - update subq time immediately
			// - fixes new game
			ReadTrack(cdr.SetSectorPlay);


			// Crusaders of Might and Magic - update getlocl now
			// - fixes cutscene speech
			{
				u8 *buf = CDR_getBuffer();
				if (buf != NULL)
					memcpy(cdr.Transfer, buf, 8);
			}

			/*
			Duke Nukem: Land of the Babes - seek then delay read for one frame
			- fixes cutscenes
			C-12 - Final Resistance - doesn't like seek
			*/

			if (cdr.Seeked != SEEK_DONE) {
				cdr.StatP |= STATUS_SEEK;
				cdr.StatP &= ~STATUS_READ;

				// Crusaders of Might and Magic - use short time
				// - fix cutscene speech (startup)

				// ??? - use more accurate seek time later
				CDREAD_INT(((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime * 1) + seekTime);
			} else {
				cdr.StatP |= STATUS_READ;
				cdr.StatP &= ~STATUS_SEEK;

				CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime * 1);
			}

			cdr.Result[0] = cdr.StatP;
			start_rotating = 1;
			break;

		default:
			CDR_LOG_I("Invalid command: %02x\n", Irq);
			error = ERROR_INVALIDCMD;
			// FALLTHROUGH

		set_error:
			SetResultSize(2);
			cdr.Result[0] = cdr.StatP | STATUS_ERROR;
			cdr.Result[1] = error;
			cdr.Stat = DiskError;
			break;
	}

	if (cdr.DriveState == DRIVESTATE_STOPPED && start_rotating) {
		cdr.DriveState = DRIVESTATE_STANDBY;
		cdr.StatP |= STATUS_ROTATING;
	}

	if (!no_busy_error) {
		switch (cdr.DriveState) {
		case DRIVESTATE_LID_OPEN:
		case DRIVESTATE_RESCAN_CD:
		case DRIVESTATE_PREPARE_CD:
			SetResultSize(2);
			cdr.Result[0] = cdr.StatP | STATUS_ERROR;
			cdr.Result[1] = ERROR_NOTREADY;
			cdr.Stat = DiskError;
			break;
		}
	}

finish:
	setIrq();
	cdr.ParamC = 0;

#ifdef CDR_LOG_CMD_IRQ
	{
		int i;
		SysPrintf("CDR IRQ %d cmd %02x stat %02x: ",
			!!(cdr.Stat & cdr.Reg2), Irq, cdr.Stat);
		for (i = 0; i < cdr.ResultC; i++)
			SysPrintf("%02x ", cdr.Result[i]);
		SysPrintf("\n");
	}
#endif
}

#ifdef HAVE_ARMV7
 #define ssat32_to_16(v) \
  asm("ssat %0,#16,%1" : "=r" (v) : "r" (v))
#else
 #define ssat32_to_16(v) do { \
  if (v < -32768) v = -32768; \
  else if (v > 32767) v = 32767; \
 } while (0)
#endif

void cdrAttenuate(s16 *buf, int samples, int stereo)
{
	int i, l, r;
	int ll = cdr.AttenuatorLeftToLeft;
	int lr = cdr.AttenuatorLeftToRight;
	int rl = cdr.AttenuatorRightToLeft;
	int rr = cdr.AttenuatorRightToRight;

	if (lr == 0 && rl == 0 && 0x78 <= ll && ll <= 0x88 && 0x78 <= rr && rr <= 0x88)
		return;

	if (!stereo && ll == 0x40 && lr == 0x40 && rl == 0x40 && rr == 0x40)
		return;

	if (stereo) {
		for (i = 0; i < samples; i++) {
			l = buf[i * 2];
			r = buf[i * 2 + 1];
			l = (l * ll + r * rl) >> 7;
			r = (r * rr + l * lr) >> 7;
			ssat32_to_16(l);
			ssat32_to_16(r);
			buf[i * 2] = l;
			buf[i * 2 + 1] = r;
		}
	}
	else {
		for (i = 0; i < samples; i++) {
			l = buf[i];
			l = l * (ll + rl) >> 7;
			//r = r * (rr + lr) >> 7;
			ssat32_to_16(l);
			//ssat32_to_16(r);
			buf[i] = l;
		}
	}
}

void cdrReadInterrupt() {
	u8 *buf;

	if (!cdr.Reading)
		return;

	if (cdr.Irq || cdr.Stat) {
		CDR_LOG_I("cdrom: read stat hack %02x %x\n", cdr.Irq, cdr.Stat);
		CDREAD_INT(0x1000);
		return;
	}

	int cdread_irq_cycles = (cdr.Mode & MODE_SPEED) ? (cdReadTime / 2) : cdReadTime;

	//senquack - Fix for Brave Fencer Musashi loading-screen freeze
	// (adapted from PCSX Reloaded)
	if ((!cdr.ReadRescheduled) && (psxHu32ref(0x1070) & psxHu32ref(0x1074) & SWAP32((u32)0x4))) {
		// HACK: emulated CPU is often slower than real thing, and
		// game may be unfinished with prev data read, so reschedule
		CDREAD_INT(cdread_irq_cycles / 2);
		cdr.ReadRescheduled = 1;
		CDR_LOG_I("cdrom: CPU BUSY, rescheduled CDREAD_INT\n");
		return;
	}

	cdr.OCUP = 1;
	SetResultSize(1);
	cdr.StatP |= STATUS_READ|STATUS_ROTATING;
	cdr.StatP &= ~STATUS_SEEK;
	cdr.Result[0] = cdr.StatP;
	cdr.Seeked = SEEK_DONE;

	ReadTrack(cdr.SetSectorPlay);

	buf = CDR_getBuffer();
	if (buf == NULL)
		cdr.RErr = -1;

	if (cdr.RErr == -1) {
		CDR_LOG_I("cdrReadInterrupt() Log: err\n");
		memset(cdr.Transfer, 0, DATA_SIZE);
		cdr.Stat = DiskError;
		cdr.Result[0] |= STATUS_ERROR;
		CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
		return;
	}

	memcpy(cdr.Transfer, buf, DATA_SIZE);

	// PPF patch file support (ppf.cpp)
	CheckPPFCache(cdr.Transfer, cdr.Prev[0], cdr.Prev[1], cdr.Prev[2]);

	CDR_LOG("cdrReadInterrupt() Log: cdr.Transfer %x:%x:%x\n", cdr.Transfer[0], cdr.Transfer[1], cdr.Transfer[2]);

	if ((!cdr.Muted) && (cdr.Mode & MODE_STRSND) && (!Config.Xa) && (cdr.FirstSector != -1)) { // CD-XA
		// Firemen 2: Multi-XA files - briefings, cutscenes
		if( cdr.FirstSector == 1 && (cdr.Mode & MODE_SF)==0 ) {
			cdr.File = cdr.Transfer[4 + 0];
			cdr.Channel = cdr.Transfer[4 + 1];
		}

		int was_first_sector = (cdr.FirstSector == 1);
		bool played_ADPCM = false;   // See comments further below

		if((cdr.Transfer[4 + 2] & 0x4) &&
			 (cdr.Transfer[4 + 1] == cdr.Channel) &&
			 (cdr.Transfer[4 + 0] == cdr.File)) {
			int ret = xa_decode_sector(&cdr.Xa, cdr.Transfer+4, cdr.FirstSector);
			if (!ret) {
				cdrAttenuate(cdr.Xa.pcm, cdr.Xa.nsamples, cdr.Xa.stereo);
				if ((cdr.Xa.nsamples != 0) && (cdr.Xa.freq != 0)) {
					played_ADPCM = true;
					SPU_playADPCMchannel(&cdr.Xa);
				}

				cdr.FirstSector = 0;
			}
			else cdr.FirstSector = -1;
		}

#ifndef SPU_NULL
		//senquack - if XA ADPCM buffer is not full, schedule next CDREAD_INT
		// IRQ twice as soon as normal, to avoid audio dropouts. Only do this
		// if there has been a XA sector with ADPCM played within the last
		// 32 calls, or else games can hang (Konami intro FMV in Castlevania
		// SOTN) in an infinite loop. Normally ADPCM sectors are interleaved
		// and every 8th,16th, or 32nd one read will be played. However, the
		// Konami logo in SOTN will read XA in a loop with no sectors ever
		// having ADPCM that gets played, and these forced IRQs here otherwise
		// would not allow its loop to escape because of its sensitive timing.
		// Don't schedule IRQs sooner on reads of first sector either, as
		// that might cause similar troubles.
		//
		// TODO: Is it also necessary to do this for CdlReadN/CdlReadS case in
		//  new cdrInterrupt() code taken from Reloaded/Rearmed?

		static unsigned int calls_since_playing_ADPCM = 0;
		if (!played_ADPCM)
			calls_since_playing_ADPCM++;
		else
			calls_since_playing_ADPCM = 0;

		if ( (Config.ForcedXAUpdates != FORCED_XA_UPDATES_OFF) &&
				(!was_first_sector) && (cdr.FirstSector != -1) &&
				(calls_since_playing_ADPCM < 32) &&
				(SPU_getADPCMBufferRoom() >= CD_FRAMESIZE_RAW*4) )
		{
			// 'Auto' setting schedules read twice as soon as normal when
			//  plugin_lib is advising next frame should be skipped.
			//  Usually works well when platform can run games at > 40fps.
			//  Does good job of syncing XA buffer to SPU buffer, reducing lag.
			if (Config.ForcedXAUpdates == FORCED_XA_UPDATES_AUTO) {
				if (pl_frameskip_advice()) cdread_irq_cycles /= 2;
			} else {
				// Don't use any advice from plugin_lib, just keep XA buffer
				//  as full as possible. Will cause FMV/speech sound lag.
				cdread_irq_cycles >>= (Config.ForcedXAUpdates - 1);
			}
		}
#endif
	}

	cdr.SetSectorPlay[2]++;
	if (cdr.SetSectorPlay[2] == 75) {
		cdr.SetSectorPlay[2] = 0;
		cdr.SetSectorPlay[1]++;
		if (cdr.SetSectorPlay[1] == 60) {
			cdr.SetSectorPlay[1] = 0;
			cdr.SetSectorPlay[0]++;
		}
	}

	cdr.Readed = 0;
	cdr.ReadRescheduled = 0;

	CDREAD_INT(cdread_irq_cycles);

	/*
	Croc 2: $40 - only FORM1 (*)
	Judge Dredd: $C8 - only FORM1 (*)
	Sim Theme Park - no adpcm at all (zero)
	*/

	if (!(cdr.Mode & MODE_STRSND) || !(cdr.Transfer[4+2] & 0x4)) {
		cdr.Stat = DataReady;
		setIrq();
	}

	// update for CdlGetlocP
	ReadTrack(cdr.SetSectorPlay);
}

/*
cdrRead0:
	bit 0 - 0 REG1 command send / 1 REG1 data read
	bit 1 - 0 data transfer finish / 1 data transfer ready/in progress
	bit 2 - unknown
	bit 3 - unknown
	bit 4 - unknown
	bit 5 - 1 result ready
	bit 6 - 1 dma ready
	bit 7 - 1 command being processed
*/

unsigned char cdrRead0(void) {
	if (cdr.ResultReady)
		cdr.Ctrl |= 0x20;
	else
		cdr.Ctrl &= ~0x20;

	if (cdr.OCUP)
		cdr.Ctrl |= 0x40;

	// What means the 0x10 and the 0x08 bits? I only saw it used by the bios
	cdr.Ctrl |= 0x18;

#ifdef CDR_LOG
	CDR_LOG("cdrRead0() Log: CD0 Read: %x\n", cdr.Ctrl);
#endif

	return psxHu8(0x1800) = cdr.Ctrl;
}

/*
cdrWrite0:
	0 - to send a command / 1 - to get the result
*/

void cdrWrite0(unsigned char rt) {
	CDR_LOG_IO("cdr w0: %02x\n", rt);

	cdr.Ctrl = (rt & 3) | (cdr.Ctrl & ~3);
}

unsigned char cdrRead1(void) {
	if ((cdr.ResultP & 0xf) < cdr.ResultC)
		psxHu8(0x1801) = cdr.Result[cdr.ResultP & 0xf];
	else
		psxHu8(0x1801) = 0;
	cdr.ResultP++;
	if (cdr.ResultP == cdr.ResultC)
		cdr.ResultReady = 0;

	CDR_LOG_IO("cdr r1: %02x\n", psxHu8(0x1801));

	return psxHu8(0x1801);
}

void cdrWrite1(unsigned char rt) {
	u8 set_loc[3];
	int i;

	CDR_LOG_IO("cdr w1: %02x\n", rt);

	switch (cdr.Ctrl & 3) {
	case 0:
		break;
	case 3:
		cdr.AttenuatorRightToRightT = rt;
		return;
	default:
		return;
	}

	cdr.Cmd = rt;
	cdr.OCUP = 0;

#ifdef CDR_LOG_CMD_IRQ
	SysPrintf("CD1 write: %x (%s)", rt, CmdName[rt]);
	if (cdr.ParamC) {
		SysPrintf(" Param[%d] = {", cdr.ParamC);
		for (i = 0; i < cdr.ParamC; i++)
			SysPrintf(" %x,", cdr.Param[i]);
		SysPrintf("}\n");
	} else {
		SysPrintf("\n");
	}
#endif

	cdr.ResultReady = 0;
	cdr.Ctrl |= 0x80;
	// cdr.Stat = NoIntr; 
	AddIrqQueue(cdr.Cmd, 0x800);

	switch (cdr.Cmd) {
	case CdlSetloc:
		for (i = 0; i < 3; i++)
			set_loc[i] = btoi(cdr.Param[i]);

		i = msf2sec(cdr.SetSectorPlay);
		i = abs(static_cast<int>(i - msf2sec(set_loc)));
		if (i > 16)
			cdr.Seeked = SEEK_PENDING;

		memcpy(cdr.SetSector, set_loc, 3);
		cdr.SetSector[3] = 0;
		cdr.SetlocPending = 1;
		break;

	case CdlReadN:
	case CdlReadS:
	case CdlPause:
		StopCdda();
		StopReading();
		break;

	case CdlReset:
	case CdlInit:
		cdr.Seeked = SEEK_DONE;
		StopCdda();
		StopReading();
		break;

    	case CdlSetmode:
		CDR_LOG("cdrWrite1() Log: Setmode %x\n", cdr.Param[0]);

        	cdr.Mode = cdr.Param[0];

		// Squaresoft on PlayStation 1998 Collector's CD Vol. 1
		// - fixes choppy movie sound
		if( cdr.Play && (cdr.Mode & MODE_CDDA) == 0 )
			StopCdda();
        	break;
	}
}

unsigned char cdrRead2(void) {
	unsigned char ret;

	if (cdr.Readed == 0) {
		ret = 0;
	} else {
		ret = *pTransfer++;
	}

#ifdef CDR_LOG
	CDR_LOG("cdrRead2() Log: CD2 Read: %x\n", ret);
#endif
	return ret;
}

void cdrWrite2(unsigned char rt) {
#ifdef CDR_LOG
	CDR_LOG("cdrWrite2() Log: CD2 write: %x\n", rt);
#endif

	switch (cdr.Ctrl & 3) {
	case 0:
		if (cdr.ParamC < 8) // FIXME: size and wrapping
			cdr.Param[cdr.ParamC++] = rt;
		return;
	case 1:
		cdr.Reg2 = rt;
		setIrq();
		return;
	case 2:
		cdr.AttenuatorLeftToLeftT = rt;
		return;
	case 3:
		cdr.AttenuatorRightToLeftT = rt;
		return;
	}
}

unsigned char cdrRead3(void) {
	if (cdr.Ctrl & 0x1)
		psxHu8(0x1803) = cdr.Stat | 0xE0;
	else
		psxHu8(0x1803) = cdr.Reg2 | 0xE0;

	CDR_LOG_IO("cdr r3: %02x\n", psxHu8(0x1803));
	return psxHu8(0x1803);
}

void cdrWrite3(unsigned char rt) {
	CDR_LOG_IO("cdr w3: %02x\n", rt);

	switch (cdr.Ctrl & 3) {
	case 0:
		break; // transfer
	case 1:
		cdr.Stat &= ~rt;

		if (rt & 0x40)
			cdr.ParamC = 0;
		return;
	case 2:
		cdr.AttenuatorLeftToRightT = rt;
		return;
	case 3:
		if (rt & 0x20) {
			memcpy(&cdr.AttenuatorLeftToLeft, &cdr.AttenuatorLeftToLeftT, 4);
			CDR_LOG_I("CD-XA Volume: %02x %02x | %02x %02x\n",
				cdr.AttenuatorLeftToLeft, cdr.AttenuatorLeftToRight,
				cdr.AttenuatorRightToLeft, cdr.AttenuatorRightToRight);
		}
		return;
	}

	if ((rt & 0x80) && cdr.Readed == 0) {
		cdr.Readed = 1;
		pTransfer = cdr.Transfer;

		switch (cdr.Mode & 0x30) {
			case MODE_SIZE_2328:
			case 0x00:
				pTransfer += 12;
				break;

			case MODE_SIZE_2340:
				pTransfer += 0;
				break;

			default:
				break;
		}
	}
}

void psxDma3(u32 madr, u32 bcr, u32 chcr) {
	u32 cdsize;
	int size;
	u8 *ptr;

	CDR_LOG("psxDma3() Log: *** DMA 3 *** %x addr = %x size = %x\n", chcr, madr, bcr);

	switch (chcr) {
		case 0x11000000:
		case 0x11400100:
			if (cdr.Readed == 0) {
				CDR_LOG("psxDma3() Log: *** DMA 3 *** NOT READY\n");
				break;
			}

			cdsize = (bcr & 0xffff) * 4;

			// Ape Escape: bcr = 0001 / 0000
			// - fix boot
			if( cdsize == 0 )
			{
				switch (cdr.Mode & (MODE_SIZE_2340|MODE_SIZE_2328)) {
					case MODE_SIZE_2340: cdsize = 2340; break;
					case MODE_SIZE_2328: cdsize = 2328; break;
					default:
					case MODE_SIZE_2048: cdsize = 2048; break;
				}
			}


			ptr = (u8 *)PSXM(madr);
			if (ptr == NULL) {
				CDR_LOG("psxDma3() Log: *** DMA 3 *** NULL Pointer!\n");
				break;
			}

			/*
			GS CDX: Enhancement CD crash
			- Setloc 0:0:0
			- CdlPlay
			- Spams DMA3 and gets buffer overrun
			*/
			size = CD_FRAMESIZE_RAW - (pTransfer - cdr.Transfer);
			if (size > cdsize)
				size = cdsize;
			if (size > 0)
			{
				memcpy(ptr, pTransfer, size);
			}

			/* Workaround for 'Studio 33' games that do Icache trickery:
			 * -senquack (mips dynarec team) January 2018
			 *  'Formula One 99'     (crash on load)
			 *                       NOTE: PAL version needs .SBI subchannel file
			 *  'Formula One 2001'   (in-game controls, AI broken, even on PS3)
			 *  'Formula One Arcade' (crash on load, PAL untested)
			 * (other Studio 33 games seem to work fine)
			 *  Dynarecs can take this opportunity to flush their code. The MIPS
			 * dynarec uses this to work around these games: flushing on icache
			 * status change alone is not enough. The complete workaround
			 * requires that recompiled code does no invalidations of its own
			 * (stores don't invalidate code). DMA transfers both here and
			 * elsewhere, as usual, do invalidate code. The workaround is only
			 * enabled for these specific games. Hacky, but fast and effective.
			 *  XXX: Shalma got these games working in PCSX Reloaded's
			 * interpreter via icache emulation. That code has not yet been
			 * backported to ours. Also, his technique would likely be slower
			 * and difficult to implement for a dynarec.
			 *  XXX: 'Formula One 2000' and 'Formula One Championship Season 2000'
			 * still suffer from freeze at startup using both this and Shalma's
			 * techniques. These are from a different publisher, however:
			 * Maybe unsupported copy-protection methods? Pure evil?
			 *  XXX: When selecting a circuit in 'Formula One 99', it takes 5-10
			 * seconds for the animated preview to finally appear. It's easy for
			 * users to mistakenly assume the game froze, even on a PC emulator.
			 * Sometimes, a range error is reported in cdrom code before this.
			 */
			if (cdr.FirstSector == 1 && size >= 8 &&
			    strncmp((const char*)pTransfer, "PS-X EXE", 8) == 0)
			{
				psxCpu->Notify(R3000ACPU_NOTIFY_DMA3_EXE_LOAD, NULL);
			}

#ifdef PSXREC
			psxCpu->Clear(madr, cdsize / 4);
#endif

			pTransfer += cdsize;

			if( chcr == 0x11400100 ) {
				HW_DMA3_MADR = SWAPu32(madr + cdsize);
				CDRDMA_INT( (cdsize/4) / 4 );
			}
			else if( chcr == 0x11000000 ) {
				// CDRDMA_INT( (cdsize/4) * 1 );
				// halted
				psxRegs.cycle += (cdsize/4) * 24/2;
				CDRDMA_INT(16);
			}
			return;

		default:
			CDR_LOG("psxDma3() Log: Unknown cddma %x\n", chcr);
			break;
	}

	HW_DMA3_CHCR &= SWAP32(~0x01000000);
	DMA_INTERRUPT(3);
}

void cdrDmaInterrupt()
{
	if (HW_DMA3_CHCR & SWAP32(0x01000000))
	{
		HW_DMA3_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(3);
	}
}

static void getCdInfo(void)
{
	u8 tmp;

	CDR_getTN(cdr.ResultTN);
	CDR_getTD(0, cdr.SetSectorEnd);
	tmp = cdr.SetSectorEnd[0];
	cdr.SetSectorEnd[0] = cdr.SetSectorEnd[2];
	cdr.SetSectorEnd[2] = tmp;
}


void cdrReset() {
	memset(&cdr, 0, sizeof(cdr));
	cdr.CurTrack = 1;
	cdr.File = 1;
	cdr.Channel = 1;
	cdr.Reg2 = 0x1f;
	cdr.Stat = NoIntr;
	cdr.DriveState = DRIVESTATE_STANDBY;
	cdr.StatP = STATUS_ROTATING;
	cdr.ReadRescheduled = 0;
	pTransfer = cdr.Transfer;

	// BIOS player - default values
	cdr.AttenuatorLeftToLeft = 0x80;
	cdr.AttenuatorLeftToRight = 0x00;
	cdr.AttenuatorRightToLeft = 0x00;
	cdr.AttenuatorRightToRight = 0x80;

	getCdInfo();
}

int cdrFreeze(void *f, FreezeMode mode)
{
	u32 tmp;
	u8 tmpp[3];

	if (mode == FREEZE_LOAD && !Config.Cdda)
		CDR_stop();
	
	cdr.freeze_ver = 0x63647202;
	if (freeze_rw(f, mode, &cdr, sizeof(cdr)))
		return -1;
	
	if (mode == FREEZE_SAVE) {
		cdr.ParamP = cdr.ParamC;
		tmp = pTransfer - cdr.Transfer;
	}

	if (freeze_rw(f, mode, &tmp, sizeof(tmp)))
		return -1;

	if (mode == FREEZE_LOAD) {
		getCdInfo();

		pTransfer = cdr.Transfer + tmp;

		// read right sub data
		memcpy(tmpp, cdr.Prev, 3);
		cdr.Prev[0]++;
		ReadTrack(tmpp);

		if (cdr.Play) {
			if (cdr.freeze_ver < 0x63647202)
				memcpy(cdr.SetSectorPlay, cdr.SetSector, 3);

			Find_CurTrack(cdr.SetSectorPlay);
			if (!Config.Cdda)
				CDR_play(cdr.SetSectorPlay);
		}

		if ((cdr.freeze_ver & 0xffffff00) != 0x63647200) {
			// old versions did not latch Reg2, have to fixup..
			if (cdr.Reg2 == 0) {
				SysPrintf("cdrom: fixing up old savestate\n");
				cdr.Reg2 = 7;
			}
			// also did not save Attenuator..
			if ((cdr.AttenuatorLeftToLeft | cdr.AttenuatorLeftToRight
			     | cdr.AttenuatorRightToLeft | cdr.AttenuatorRightToRight) == 0)
			{
				cdr.AttenuatorLeftToLeft = cdr.AttenuatorRightToRight = 0x80;
			}
		}
	}

	return 0;
}

void LidInterrupt() {
	getCdInfo();
	StopCdda();
	cdrLidSeekInterrupt();
}
