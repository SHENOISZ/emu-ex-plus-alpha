/*  This file is part of MSX.emu.

	MSX.emu is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	MSX.emu is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with MSX.emu.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "main"
#include <emuframework/EmuApp.hh>
#include <emuframework/EmuAppInlines.hh>
#include <imagine/fs/ArchiveFS.hh>
#include <imagine/gui/AlertView.hh>
#include "internal.hh"

// TODO: remove when namespace code is complete
#ifdef __APPLE__
#define Fixed MacTypes_Fixed
#define Rect MacTypes_Rect
#include <MacTypes.h>
#undef Fixed
#undef Rect
#endif

extern "C"
{
	#include <blueMSX/IoDevice/Casette.h>
	#include <blueMSX/IoDevice/Disk.h>
	#include <blueMSX/Input/JoystickPort.h>
	#include <blueMSX/Board/Board.h>
	#include <blueMSX/Board/MSX.h>
	#include <blueMSX/Board/Coleco.h>
	#include <blueMSX/Language/Language.h>
	#include <blueMSX/Z80/R800.h>
	#include <blueMSX/Memory/MegaromCartridge.h>
	#include <blueMSX/Input/InputEvent.h>
	#include <blueMSX/Utils/SaveState.h>
}

#include <blueMSX/Utils/ziphelper.h>

const char *EmuSystem::creditsViewStr = CREDITS_INFO_STRING "(c) 2011-2014\nRobert Broglia\nwww.explusalpha.com\n\nPortions (c) the\nBlueMSX Team\nbluemsx.com";
bool EmuSystem::handlesGenericIO = false; // TODO: need to re-factor BlueMSX file loading code
BoardInfo boardInfo{};
Machine *machine{};
Mixer *mixer{};
bool canInstallCBIOS = true;
FS::PathString machineCustomPath{};
FS::PathString machineBasePath{};
FS::FileString cartName[2]{};
extern RomType currentRomType[2];
FS::FileString diskName[2]{};
static FS::FileString tapeName{};
FS::FileString hdName[4]{};
static const uint msxMaxResX = (256) * 2, msxResY = 224,
	msxMaxFrameBuffResX = (272) * 2, msxMaxFrameBuffResY = 240;
static int msxResX = msxMaxResX/2;
static constexpr auto pixFmt = IG::PIXEL_FMT_RGB565;
static uint16 screenBuff[msxMaxFrameBuffResX*msxMaxFrameBuffResY] __attribute__ ((aligned (8))) {0};
static char *srcPixData = (char*)&screenBuff[8 * msxResX];
static IG::Pixmap srcPix{{{msxResX, msxResY}, pixFmt}, srcPixData};
static bool doubleWidthFrame = false;
static EmuVideo *emuVideo{};

#if defined CONFIG_BASE_ANDROID || defined CONFIG_ENV_WEBOS || defined CONFIG_BASE_IOS || defined CONFIG_MACHINE_IS_PANDORA
static const bool checkForMachineFolderOnStart = true;
#else
static const bool checkForMachineFolderOnStart = false;
#endif

CLINK Int16 *mixerGetBuffer(Mixer* mixer, UInt32 *samplesOut);

FS::PathString makeMachineBasePath(FS::PathString customPath)
{
	FS::PathString outPath;
	if(!strlen(customPath.data()))
	{
		#if defined CONFIG_ENV_LINUX && !defined CONFIG_MACHINE_PANDORA
		string_printf(outPath, "%s/MSX.emu", Base::assetPath().data());
		#else
		string_printf(outPath, "%s/MSX.emu", Base::storagePath().data());
		#endif
	}
	else
	{
		string_printf(outPath, "%s", customPath.data());
	}
	logMsg("set machine file path: %s", outPath.data());
	return outPath;
}

const char *machineBasePathStr()
{
	return machineBasePath.data();
}

bool hasMSXTapeExtension(const char *name)
{
	return string_hasDotExtension(name, "cas");
}

bool hasMSXDiskExtension(const char *name)
{
	return string_hasDotExtension(name, "dsk");
}

bool hasMSXROMExtension(const char *name)
{
	return string_hasDotExtension(name, "rom") || string_hasDotExtension(name, "mx1")
			|| string_hasDotExtension(name, "mx2") || string_hasDotExtension(name, "col");
}

static bool hasMSXExtension(const char *name)
{
	return hasMSXROMExtension(name) || hasMSXDiskExtension(name);
}

const char *EmuSystem::shortSystemName()
{
	return "MSX";
}

const char *EmuSystem::systemName()
{
	return "MSX";
}

EmuSystem::NameFilterFunc EmuSystem::defaultFsFilter = hasMSXExtension;
EmuSystem::NameFilterFunc EmuSystem::defaultBenchmarkFsFilter = hasMSXExtension;

// frameBuffer implementation
Pixel* frameBufferGetLine(FrameBuffer* frameBuffer, int y)
{
	//logMsg("getting line %d", y);
	/*if(y < 8 || y >= (224+8)) return dummyLine;
    return (Pixel*)&screenBuff[(y - 8) * msxResX];*/
	return (Pixel*)&screenBuff[y * msxResX];
}

int frameBufferGetDoubleWidth(FrameBuffer* frameBuffer, int y)
{
	return doubleWidthFrame;
}

void frameBufferSetDoubleWidth(FrameBuffer* frameBuffer, int y, int val)
{
	if(doubleWidthFrame != val)
	{
		logMsg("setting double width line %d, %d", y, val);
		doubleWidthFrame = val;
		msxResX = doubleWidthFrame ? msxMaxResX : msxMaxResX/2;
		srcPix = {{{msxResX, msxResY}, pixFmt}, srcPixData};
	}
}

static bool insertMedia()
{
	iterateTimes(2, i)
	{
		switch(currentRomType[i])
		{
			bcase ROM_SCC: logMsg("loading SCC"); boardChangeCartridge(i, ROM_SCC, "", 0);
			bcase ROM_SCCPLUS: logMsg("loading SCC+"); boardChangeCartridge(i, ROM_SCCPLUS, "", 0);
			bcase ROM_SUNRISEIDE:
				logMsg("loading Sunrise IDE");
				if(!boardChangeCartridge(i, ROM_SUNRISEIDE, "Sunrise IDE", 0))
				{
					EmuApp::postMessage(true, "Error loading Sunrise IDE device");
				}
			bdefault:
			{
				bool exists = strlen(cartName[i].data());
				if(exists)
					logMsg("loading ROM %s", cartName[i].data());
				if(exists && !insertROM(cartName[i].data(), i))
				{
					EmuApp::printfMessage(3, 1, "Error loading ROM%d:\n%s", i, cartName[i].data());
					return 0;
				}
			}
		}
	}

	iterateTimes(2, i)
	{
		bool exists = strlen(diskName[i].data());
		if(exists)
			logMsg("loading Disk %s", diskName[i].data());
		if(exists && !insertDisk(diskName[i].data(), i))
		{
			EmuApp::printfMessage(3, 1, "Error loading Disk%d:\n%s", i, diskName[i].data());
			return 0;
		}
	}

	iterateTimes(4, i)
	{
		bool exists = strlen(hdName[i].data());
		if(exists)
			logMsg("loading HD %s", hdName[i].data());
		if(exists && !insertDisk(hdName[i].data(), diskGetHdDriveId(i / 2, i % 2)))
		{
			EmuApp::printfMessage(3, 1, "Error loading Disk%d:\n%s", i, hdName[i].data());
			return 0;
		}
	}
	return 1;
}

static bool msxIsInit()
{
	return boardInfo.run != 0;
}

static void clearAllMediaNames()
{
	cartName[0] = {};
	cartName[1] = {};
	diskName[0] = {};
	diskName[1] = {};
	hdName[0] = {};
	hdName[1] = {};
	hdName[2] = {};
	hdName[3] = {};
	tapeName = {};
}

static void ejectMedia()
{
	boardChangeCartridge(0, ROM_UNKNOWN, 0, 0);
	boardChangeCartridge(1, ROM_UNKNOWN, 0, 0);
	diskChange(0, 0, 0);
	diskChange(1, 0, 0);
	diskChange(diskGetHdDriveId(0, 0), 0, 0);
	diskChange(diskGetHdDriveId(0, 1), 0, 0);
	diskChange(diskGetHdDriveId(1, 0), 0, 0);
	diskChange(diskGetHdDriveId(1, 1), 0, 0);
}

static void destroyMSX()
{
	assert(machine);
	logMsg("destroying MSX");
	fdcActive = 0;
	if(msxIsInit())
	{
		ejectMedia();
		boardInfo.destroy();
	}
	boardInfo = {};
	clearAllMediaNames();
}

static const char *boardTypeToStr(BoardType type)
{
	switch(type)
	{
    case BOARD_MSX:
    case BOARD_MSX_S3527:
    case BOARD_MSX_S1985:
    case BOARD_MSX_T9769B:
    case BOARD_MSX_T9769C:
    case BOARD_MSX_FORTE_II:
    	return "MSX";
    case BOARD_COLECO:
    	return "Coleco";
    default:
    	return "Unknown";
	}
}

static bool createBoard()
{
	// TODO: 50hz mode
	assert(machine);
	switch (machine->board.type)
	{
		case BOARD_MSX:
		case BOARD_MSX_S3527:
		case BOARD_MSX_S1985:
		case BOARD_MSX_T9769B:
		case BOARD_MSX_T9769C:
		case BOARD_MSX_FORTE_II:
			logMsg("creating MSX");
			joystickPortSetType(0, JOYSTICK_PORT_JOYSTICK);
			joystickPortSetType(1, JOYSTICK_PORT_JOYSTICK);
			activeBoardType = BOARD_MSX;
			setupVKeyboardMap(BOARD_MSX);
			return msxCreate(machine, VDP_SYNC_60HZ, &boardInfo);
		case BOARD_COLECO:
			logMsg("creating Coleco");
			joystickPortSetType(0, JOYSTICK_PORT_COLECOJOYSTICK);
			joystickPortSetType(1, JOYSTICK_PORT_COLECOJOYSTICK);
			activeBoardType = BOARD_COLECO;
			setupVKeyboardMap(BOARD_COLECO);
			return colecoCreate(machine, VDP_SYNC_60HZ, &boardInfo);
		default:
			logErr("error: unknown board type 0x%X", machine->board.type);
			return 0;
	}
}

static bool createBoardFromLoadGame()
{
	if(msxIsInit())
		destroyMSX();
	if(!createBoard())
	{
		boardInfo = {};
		return false;
	}
	//logMsg("z80 freq %d, r800 %d", ((R800*)boardInfo.cpuRef)->frequencyZ80, ((R800*)boardInfo.cpuRef)->frequencyR800);
	logMsg("max carts %d, disks %d, tapes %d", boardInfo.cartridgeCount, boardInfo.diskdriveCount, boardInfo.casetteCount);
	return true;
}

static EmuSystem::Error makeMachineInitError(const char *machineName)
{
	return EmuSystem::makeError("Error loading machine files for\n\"%s\",\nmake sure they are in:\n%s", machineName, machineBasePath.data());
}

static bool initMachine(const char *machineName)
{
	if(machine && string_equal(machine->name, machineName))
	{
		return true;
	}
	logMsg("loading machine %s", machineName);
	if(machine)
		machineDestroy(machine);
	FS::current_path(machineBasePathStr());
	machine = machineCreate(machineName);
	if(!machine)
	{
		return false;
	}
	boardSetMachine(machine);
	return true;
}

template<class MATCH_FUNC>
static bool getFirstFilenameInZip(const char *zipPath, char *nameOut, uint outBytes, MATCH_FUNC nameMatch)
{
	assert(nameOut);
	ArchiveIO io{};
	std::error_code ec{};
	for(auto &entry : FS::ArchiveIterator{zipPath, ec})
	{
		if(entry.type() == FS::file_type::directory)
		{
			continue;
		}
		auto name = entry.name();
		logMsg("archive file entry:%s", entry.name());
		if(nameMatch(name))
		{
			string_copy(nameOut, name, outBytes);
			return true;
		}
	}
	if(ec)
	{
		logErr("error opening archive:%s", zipPath);
	}
	return false;
}

static bool getFirstROMFilenameInZip(const char *zipPath, char *nameOut, uint outBytes)
{
	return getFirstFilenameInZip(zipPath, nameOut, outBytes, hasMSXROMExtension);
}

static bool getFirstDiskFilenameInZip(const char *zipPath, char *nameOut, uint outBytes)
{
	return getFirstFilenameInZip(zipPath, nameOut, outBytes, hasMSXDiskExtension);
}

static bool getFirstTapeFilenameInZip(const char *zipPath, char *nameOut, uint outBytes)
{
	return getFirstFilenameInZip(zipPath, nameOut, outBytes, hasMSXTapeExtension);
}

bool insertROM(const char *name, uint slot)
{
	auto path = FS::makePathString(EmuSystem::gamePath(), name);
	char fileInZipName[256] = "";
	if(string_hasDotExtension(path.data(), "zip"))
	{
		if(!getFirstROMFilenameInZip(path.data(), fileInZipName, sizeof(fileInZipName)))
		{
			EmuApp::postMessage(true, "No ROM found in zip");
			return false;
		}
		logMsg("found %s in zip", fileInZipName);
	}
	if(!boardChangeCartridge(slot, ROM_UNKNOWN, path.data(), strlen(fileInZipName) ? fileInZipName : 0))
	{
		EmuApp::postMessage(true, "Error loading ROM");
		return false;
	}
	return true;
}

bool insertDisk(const char *name, uint slot)
{
	auto path = FS::makePathString(EmuSystem::gamePath(), name);
	char fileInZipName[256] = "";
	if(string_hasDotExtension(path.data(), "zip"))
	{
		if(!getFirstDiskFilenameInZip(path.data(), fileInZipName, sizeof(fileInZipName)))
		{
			EmuApp::postMessage(true, "No Disk found in zip");
			return false;
		}
		logMsg("found %s in zip", fileInZipName);
	}
	if(!diskChange(slot, path.data(), strlen(fileInZipName) ? fileInZipName : 0))
	{
		EmuApp::postMessage(true, "Error loading Disk");
		return false;
	}
	return true;
}

void EmuSystem::reset(ResetMode mode)
{
	assert(gameIsRunning());
	fdcActive = 0;
	//boardInfo.softReset();
	boardInfo.destroy();
	if(!createBoard())
	{
		boardInfo = {};
		EmuApp::postMessage(true, "Error during MSX reset");
		destroyMSX();
	}
	insertMedia();
}

FS::PathString EmuSystem::sprintStateFilename(int slot, const char *statePath, const char *gameName)
{
	return FS::makePathStringPrintf("%s/%s.0%c.sta", statePath, gameName, saveSlotCharUpper(slot));
}

static const char saveStateVersion[] = "blueMSX - state  v 8";
extern int pendingInt;

static EmuSystem::Error saveBlueMSXState(const char *filename)
{
	CallResult res = zipStartWrite(filename);
	if(res != OK)
	{
		logErr("error creating zip:%s", filename);
		return EmuSystem::makeFileWriteError();
	}
	saveStateCreateForWrite(filename);
	int rv = zipSaveFile(filename, "version", 0, saveStateVersion, sizeof(saveStateVersion));
	if (!rv)
	{
		saveStateDestroy();
		zipEndWrite();
		logErr("error writing to zip:%s", filename);
		return EmuSystem::makeFileWriteError();
	}

	SaveState* state = saveStateOpenForWrite("board");

	saveStateSet(state, "pendingInt", pendingInt);
	saveStateSet(state, "cartType00", currentRomType[0]);
	if(strlen(cartName[0].data()))
		saveStateSetBuffer(state, "cartName00",  cartName[0].data(), strlen(cartName[0].data()) + 1);
	saveStateSet(state, "cartType01", currentRomType[1]);
	if(strlen(cartName[1].data()))
		saveStateSetBuffer(state, "cartName01",  cartName[1].data(), strlen(cartName[1].data()) + 1);
	if(strlen(diskName[0].data()))
		saveStateSetBuffer(state, "diskName00",  diskName[0].data(), strlen(diskName[0].data()) + 1);
	if(strlen(diskName[1].data()))
		saveStateSetBuffer(state, "diskName01",  diskName[1].data(), strlen(diskName[1].data()) + 1);
	if(strlen(hdName[0].data()))
		saveStateSetBuffer(state, "diskName02",  hdName[0].data(), strlen(hdName[0].data()) + 1);
	if(strlen(hdName[1].data()))
		saveStateSetBuffer(state, "diskName03",  hdName[1].data(), strlen(hdName[1].data()) + 1);
	if(strlen(hdName[2].data()))
		saveStateSetBuffer(state, "diskName10",  hdName[2].data(), strlen(hdName[2].data()) + 1);
	if(strlen(hdName[3].data()))
		saveStateSetBuffer(state, "diskName11",  hdName[3].data(), strlen(hdName[3].data()) + 1);
	saveStateClose(state);

	machineSaveState(machine);
	boardInfo.saveState();
	saveStateDestroy();
	zipEndWrite();
	return {};
}

EmuSystem::Error EmuSystem::saveState(const char *path)
{
	return saveBlueMSXState(path);
}

template <typename T>
static void saveStateGetFileString(SaveState* state, const char* tagName, T &dest)
{
	saveStateGetBuffer(state, tagName,  dest.data(), dest.size());
	if(strlen(dest.data()))
	{
		// strip any file path
		dest = FS::basename(dest);
	}
}

static EmuSystem::Error loadBlueMSXState(const char *filename)
{
	logMsg("loading state %s", filename);

	assert(machine);
	ejectMedia();

	saveStateCreateForRead(filename);
	int size;
	char *version = (char*)zipLoadFile(filename, "version", &size);
	if(!version)
	{
		saveStateDestroy();
		return EmuSystem::makeFileReadError();
	}
	if(0 != strncmp(version, saveStateVersion, sizeof(saveStateVersion) - 1))
	{
		free(version);
		saveStateDestroy();
		return EmuSystem::makeError("Incorrect state version");
	}
	free(version);

	machineLoadState(machine);
	logMsg("machine name %s", machine->name);
	char optionMachineNameStrOld[128];
	string_copy(optionMachineNameStrOld, optionMachineNameStr);
	string_copy(optionMachineNameStr, machine->name);

	// from this point on, errors are fatal and require the existing game to close
	if(!createBoardFromLoadGame())
	{
		saveStateDestroy();
		string_copy(optionMachineNameStr, optionMachineNameStrOld); // restore old machine name
		EmuApp::exitGame(false);
		return EmuSystem::makeError("Invalid data in file");
	}

	clearAllMediaNames();
	SaveState* state = saveStateOpenForRead("board");
	currentRomType[0] = saveStateGet(state, "cartType00", 0);
	saveStateGetFileString(state, "cartName00",  cartName[0]);
	currentRomType[1] = saveStateGet(state, "cartType01", 0);
	saveStateGetFileString(state, "cartName01",  cartName[1]);
	saveStateGetFileString(state, "diskName00",  diskName[0]);
	saveStateGetFileString(state, "diskName01",  diskName[1]);
	saveStateGetFileString(state, "diskName02",  hdName[0]);
	saveStateGetFileString(state, "diskName03",  hdName[1]);
	saveStateGetFileString(state, "diskName10",  hdName[2]);
	saveStateGetFileString(state, "diskName11",  hdName[3]);
	saveStateClose(state);

	if(!insertMedia())
	{
		EmuApp::exitGame(false);
		return EmuSystem::makeError("Error loading media");
	}

	boardInfo.loadState();
	saveStateDestroy();
	srcPix = {{{msxResX, msxResY}, pixFmt}, srcPixData};
	return {};
}

EmuSystem::Error EmuSystem::loadState(const char *path)
{
	return loadBlueMSXState(path);
}

void EmuSystem::saveBackupMem()
{
	if(gameIsRunning())
	{
		// TODO: add BlueMSX API to flush volatile data
	}
}

void EmuSystem::closeSystem()
{
	destroyMSX();
}

EmuSystem::Error EmuSystem::loadGame(IO &, OnLoadProgressDelegate)
{
	srcPix = {{{msxResX, msxResY}, pixFmt}, srcPixData};

	if(!initMachine(optionMachineName)) // make sure machine is set to user's selection
	{
		return makeMachineInitError(optionMachineName);
	}

	if(!createBoardFromLoadGame())
	{
		return EmuSystem::makeError("Error initializing %s", machine->name);
	}

	char fileInZipName[256] = "";
	if(string_hasDotExtension(gameFileName().data(), "zip"))
	{
		if(getFirstROMFilenameInZip(fullGamePath(), fileInZipName, sizeof(fileInZipName)))
		{
			logMsg("found %s in zip", fileInZipName);
			cartName[0] = gameFileName();
			if(!boardChangeCartridge(0, ROM_UNKNOWN, fullGamePath(), fileInZipName))
			{
				destroyMSX();
				return EmuSystem::makeError("Error loading ROM");
			}
		}
		else if(getFirstDiskFilenameInZip(fullGamePath(), fileInZipName, sizeof(fileInZipName)))
		{
			logMsg("found %s in zip", fileInZipName);
			auto fileInZip = FS::fileFromArchive(fullGamePath(), fileInZipName);
			bool loadAsHD = fileInZip.size() >= 1024 * 1024;
			fileInZip.close();
			if(loadAsHD)
			{
				logMsg("load disk as HD");
				int hdId = diskGetHdDriveId(0, 0);
				string_copy(cartName[0], "Sunrise IDE");
				if(!boardChangeCartridge(0, ROM_SUNRISEIDE, "Sunrise IDE", 0))
				{
					destroyMSX();
					return EmuSystem::makeError("Error loading Sunrise IDE device");
				}
				hdName[0] = gameFileName();
				if(!diskChange(hdId, fullGamePath(), fileInZipName))
				{
					destroyMSX();
					return EmuSystem::makeError("Error loading HD");
				}
			}
			else
			{
				diskName[0] = gameFileName();
				if(!diskChange(0, fullGamePath(), fileInZipName))
				{
					destroyMSX();
					return EmuSystem::makeError("Error loading Disk");
				}
			}
		}
		else
		{
			destroyMSX();
			return EmuSystem::makeError("No Media in ZIP");
		}
	}
	else if(hasMSXROMExtension(gameFileName().data()))
	{
		cartName[0] = gameFileName();
		if(!boardChangeCartridge(0, ROM_UNKNOWN, fullGamePath(), 0))
		{
			destroyMSX();
			return EmuSystem::makeError("Error loading ROM");
		}
	}
	else if(hasMSXDiskExtension(gameFileName().data()))
	{
		bool loadAsHD = FS::file_size(fullGamePath()) >= 1024 * 1024;
		if(loadAsHD)
		{
			logMsg("load disk as HD");
			int hdId = diskGetHdDriveId(0, 0);
			string_copy(cartName[0], "Sunrise IDE");
			if(!boardChangeCartridge(0, ROM_SUNRISEIDE, "Sunrise IDE", 0))
			{
				destroyMSX();
				return EmuSystem::makeError("Error loading Sunrise IDE device");
			}
			hdName[0] = gameFileName();
			if(!diskChange(hdId, fullGamePath(), 0))
			{
				destroyMSX();
				return EmuSystem::makeError("Error loading HD");
			}
		}
		else
		{
			diskName[0] = gameFileName();
			if(!diskChange(0, fullGamePath(), 0))
			{
				destroyMSX();
				return EmuSystem::makeError("Error loading Disk");
			}
		}
	}
	else
	{
		return EmuSystem::makeError("Unknown file type");
	}
	return {};
}

void EmuSystem::configAudioRate(double frameTime, int rate)
{
	assumeExpr(rate == 44100);// TODO: not all sound chips handle non-44100Hz sample rate
	uint mixRate = std::round(rate * (59.924 * frameTime));
	mixerSetSampleRate(mixer, mixRate);
	logMsg("set mixer rate %d", (int)mixerGetSampleRate(mixer));
}

static Int32 soundWrite(void* dummy, Int16 *buffer, UInt32 count)
{
	//logMsg("called audio callback %d samples", count);
	bug_unreachable("should never be called");
	return 0;
}

static void commitVideoFrame()
{
	if(likely(emuVideo))
	{
		emuVideo->setFormat({{msxResX, msxResY}, pixFmt});
		emuVideo->writeFrame(srcPix);
		EmuApp::updateAndDrawEmuVideo();
		emuVideo = {};
	}
}

void RefreshScreen(int screenMode)
{
	//logMsg("called RefreshScreen");
	commitVideoFrame();
	boardInfo.stop(boardInfo.cpuRef);
}

void EmuSystem::runFrame(EmuVideo &video, bool renderGfx, bool processGfx, bool renderAudio)
{
	// fast-forward during floppy access, but stop if access ends
	if(unlikely(fdcActive && renderGfx))
	{
		iterateTimes(4, i)
		{
			boardInfo.run(boardInfo.cpuRef);
			((R800*)boardInfo.cpuRef)->terminate = 0;
			bool useFrame = !fdcActive;
			if(useFrame)
			{
				logMsg("FDC activity ended while fast-forwarding");
				emuVideo = &video;
				commitVideoFrame();
			}
			mixerSync(mixer);
			UInt32 samples;
			uchar *audio = (uchar*)mixerGetBuffer(mixer, &samples);
			if(useFrame)
			{
				writeSound(audio, samples/2);
				return; // done with frame for this update
			}
		}
	}

	// regular frame update
	if(renderGfx)
		emuVideo = &video;
	boardInfo.run(boardInfo.cpuRef);
	((R800*)boardInfo.cpuRef)->terminate = 0;
	mixerSync(mixer);
	UInt32 samples;
	uchar *audio = (uchar*)mixerGetBuffer(mixer, &samples);
	//logMsg("%d samples", samples/2);
	if(renderAudio && samples)
	{
		writeSound(audio, samples/2);
	}
}

void EmuApp::onCustomizeNavView(EmuApp::NavView &view)
{
	const Gfx::LGradientStopDesc navViewGrad[] =
	{
		{ .0, Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
		{ .03, Gfx::VertexColorPixelFormat.build((127./255.) * .4, (255./255.) * .4, (212./255.) * .4, 1.) },
		{ .3, Gfx::VertexColorPixelFormat.build((127./255.) * .4, (255./255.) * .4, (212./255.) * .4, 1.) },
		{ .97, Gfx::VertexColorPixelFormat.build((42./255.) * .4, (85./255.) * .4, (85./255.) * .4, 1.) },
		{ 1., Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
	};
	view.setBackgroundGradient(navViewGrad);
}

void EmuApp::onMainWindowCreated(ViewAttachParams attach, Input::Event e)
{
	if(canInstallCBIOS && checkForMachineFolderOnStart &&
		!strlen(machineCustomPath.data()) && !FS::exists(machineBasePath)) // prompt to install if using default machine path & it doesn't exist
	{
		pushAndShowNewYesNoAlertView(attach, e,
			installFirmwareFilesMessage,
			"Yes", "No",
			[](TextMenuItem &, View &view, Input::Event)
			{
				view.dismiss();
				installFirmwareFiles();
			}, {});
	}
};

EmuSystem::Error EmuSystem::onInit()
{
	/*mediaDbCreateRomdb();
	mediaDbAddFromXmlFile("msxromdb.xml");
	mediaDbAddFromXmlFile("msxsysromdb.xml");*/

	#ifdef CONFIG_BASE_IOS
	if(!Base::isSystemApp())
	{
		canInstallCBIOS = false;
	}
	#endif

	// must create the mixer first since mainInitCommon() will access it
	mixer = mixerCreate();
	assert(mixer);

	// Init general emu
	langInit();
	videoManagerReset();
	tapeSetReadOnly(1);
	mediaDbSetDefaultRomType(ROM_UNKNOWN);

	// Init Mixer
	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_PSG, 100);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_PSG, 50/*40*/);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_PSG, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_SCC, 100);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_SCC, 50/*60*/);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_SCC, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_MSXMUSIC, 95);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_MSXMUSIC, 50/*60*/);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_MSXMUSIC, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_MSXAUDIO, 95);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_MSXAUDIO, 50);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_MSXAUDIO, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_MOONSOUND, 95);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_MOONSOUND, 50);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_MOONSOUND, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_YAMAHA_SFG, 95);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_YAMAHA_SFG, 50);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_YAMAHA_SFG, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_PCM, 95);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_PCM, 50);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_PCM, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_IO, 50);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_IO, 50/*70*/);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_IO, 0);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_MIDI, 90);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_MIDI, 50);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_MIDI, 1);

	mixerSetChannelTypeVolume(mixer, MIXER_CHANNEL_KEYBOARD, 65);
	mixerSetChannelTypePan(mixer, MIXER_CHANNEL_KEYBOARD, 50/*55*/);
	mixerEnableChannelType(mixer, MIXER_CHANNEL_KEYBOARD, 1);

	//mixerSetMasterVolume(mixer, 100);
	mixerSetStereo(mixer, 1);
	mixerEnableMaster(mixer, 1);
	int logFrequency = 50;
	int frequency = (int)(3579545 * ::pow(2.0, (logFrequency - 50) / 15.0515));
	mixerSetBoardFrequencyFixed(frequency);
	mixerSetWriteCallback(mixer, 0, 0, 10000);

	return {};
}
