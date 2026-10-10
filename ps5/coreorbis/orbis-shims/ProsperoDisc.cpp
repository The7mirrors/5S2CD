// Physical PS2/PS1 discs on the PS5 (2026-10-09, AI-assisted): the console's own drive is /dev/cd0 and a USB DVD
// drive shows up as /dev/cd1 (FreeBSD cd(4) through CAM). A payload-tested probe read PS2 discs from both with plain
// 2048-byte pread()s on whole sectors, CDs included, so this follows PCSX2's Darwin/FreeBSD IOCtlSrc: DVD layout
// from DVDIOCREADSTRUCTURE, CD tracks from CDIOREADTOCENTRYS, user data with pread(). Raw 2352-byte reads have no
// cd(4) ioctl for data tracks, so a data sector is rebuilt around its 2048 bytes (Mode 2 Form 1 header, which is what
// PS2 and PS1 CDs use); audio sectors are silence, as cd(4) can't read them.
#include "CDVD/CDVDdiscReader.h"
#include "CDVD/CDVD.h"

#include "common/Console.h"
#include "common/Error.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/cdio.h>
#include <sys/disk.h>
#include <sys/dvdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
	if (m_device != -1)
		close(m_device);
}

bool IOCtlSrc::Reopen(Error* error)
{
	if (m_device != -1)
		close(m_device);
	m_toc.clear();
	m_sectors = 0;
	m_layer_break = 0;
	m_media_type = 0;

	m_device = open(m_filename.c_str(), O_RDONLY);
	if (m_device == -1)
	{
		Error::SetErrno(error, errno);
		Console.Error("CDVD: open %s failed: %s", m_filename.c_str(), strerror(errno));
		return false;
	}

	// DVD first: the TOC ioctls answer for DVDs too (one data track).
	if (!ReadDVDInfo() && !ReadCDInfo())
		Console.Warning("CDVD: %s: no disc layout yet (empty drive?)", m_filename.c_str());
	else
		Console.WriteLn("CDVD: %s: %s, %u sectors, layer break %u, %zu tracks", m_filename.c_str(), m_media_type >= 0 ? "DVD" : "CD",
			m_sectors, m_layer_break, m_toc.size());
	return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
	(void)restore_defaults;
}

u32 IOCtlSrc::GetSectorCount() const
{
	return m_sectors;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return m_layer_break;
}

s32 IOCtlSrc::GetMediaType() const
{
	return m_media_type;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	return m_toc;
}

// Read-ahead (2026-10-09): the PS5's own drive reads CDs slower than a USB drive and seeks longer, so a game's menu that
// loads many small pieces in a row waited on it (emulation 37-88% for 4 s from /dev/cd0, ~100% from /dev/cd1). A miss
// reads the next 64 sectors (128 KB) in one go and keeps them; the reads that follow come from memory.
namespace
{
std::mutex s_ahead_mutex;
int s_ahead_fd = -1;
u32 s_ahead_start = 0, s_ahead_count = 0, s_ahead_total = 0; // total: the disc size it was read from (a swapped disc misses)
std::vector<u8> s_ahead(64 * 2048);
constexpr u32 kAheadSectors = 64;
} // namespace

static bool ReadDirect(int fd, u32 sector, u32 count, u8* buffer)
{
	const ssize_t want = 2048 * static_cast<ssize_t>(count);
	const ssize_t got = pread(fd, buffer, want, sector * 2048ULL);
	if (got == want)
		return true;
	if (got == -1)
		DevCon.Warning("CDVD: read sectors %u-%u failed: %s", sector, sector + count - 1, strerror(errno));
	else
		DevCon.Warning("CDVD: read sectors %u-%u: %zd of %zd bytes", sector, sector + count - 1, got, want);
	return false;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	if (count < kAheadSectors && (!m_sectors || sector + count <= m_sectors))
	{
		std::lock_guard<std::mutex> lock(s_ahead_mutex);
		if (s_ahead_fd != m_device || s_ahead_total != m_sectors || sector < s_ahead_start || sector + count > s_ahead_start + s_ahead_count)
		{
			const u32 n = m_sectors ? std::min<u32>(kAheadSectors, m_sectors - sector) : kAheadSectors;
			s_ahead_fd = -1;
			if (n >= count && ReadDirect(m_device, sector, n, s_ahead.data()))
			{
				s_ahead_fd = m_device;
				s_ahead_start = sector;
				s_ahead_count = n;
				s_ahead_total = m_sectors;
			}
		}
		if (s_ahead_fd == m_device && s_ahead_total == m_sectors && sector >= s_ahead_start && sector + count <= s_ahead_start + s_ahead_count)
		{
			std::memcpy(buffer, s_ahead.data() + (sector - s_ahead_start) * 2048ULL, count * 2048ULL);
			return true;
		}
	}
	return ReadDirect(m_device, sector, count, buffer);
}

[[maybe_unused]] static bool ReadSectors2048Unbuffered(int m_device, u32 sector, u32 count, u8* buffer)
{
	const ssize_t want = 2048 * static_cast<ssize_t>(count);
	const ssize_t got = pread(m_device, buffer, want, sector * 2048ULL);
	if (got == want)
		return true;
	if (got == -1)
		DevCon.Warning("CDVD: read sectors %u-%u failed: %s", sector, sector + count - 1, strerror(errno));
	else
		DevCon.Warning("CDVD: read sectors %u-%u: %zd of %zd bytes", sector, sector + count - 1, got, want);
	return false;
}

static void LbaToMsfBcd(u32 lba, u8* out)
{
	lba += 150;
	const u32 m = lba / (60 * 75), s = (lba / 75) % 60, f = lba % 75;
	auto bcd = [](u32 v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); };
	out[0] = bcd(m);
	out[1] = bcd(s);
	out[2] = bcd(f);
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	for (u32 i = 0; i < count; ++i)
	{
		const u32 lba = sector + i;
		u8* out = buffer + i * 2352;
		bool audio = false;
		for (size_t t = 0; t < m_toc.size(); ++t)
		{
			const u32 end = t + 1 < m_toc.size() ? m_toc[t + 1].lba : m_sectors;
			if (lba >= m_toc[t].lba && lba < end)
				audio = (m_toc[t].control & 0x04) == 0;
		}
		if (audio)
		{
			// cd(4) has no audio-read ioctl (FreeBSD dropped CDIOCREADAUDIO); CD music tracks play as silence for now.
			std::memset(out, 0, 2352);
			continue;
		}
		// Data: sync, header (MSF + mode 2), XA subheader for Form 1, the 2048 user bytes; EDC/ECC left zero.
		std::memset(out, 0, 2352);
		std::memset(out + 1, 0xFF, 10);
		LbaToMsfBcd(lba, out + 12);
		out[15] = 2;
		out[18] = out[22] = 0x08;
		if (!ReadSectors2048(lba, 1, out + 24))
			return false;
	}
	return true;
}

bool IOCtlSrc::ReadDVDInfo()
{
	dvd_struct ds = {};
	ds.format = DVD_STRUCT_PHYSICAL;
	ds.layer_num = 0;
	if (ioctl(m_device, DVDIOCREADSTRUCTURE, &ds) == -1)
		return false;
	const dvd_layer* l0 = reinterpret_cast<const dvd_layer*>(ds.data);
	const u32 start = l0->start_sector, end = l0->end_sector;
	// The PS5's drive answers this ioctl for CDs too (Rally Fusion, a PS2 CD, came back as a "DVD"): a DVD-ROM's data area
	// always starts at physical sector 0x30000, so anything else isn't a DVD.
	if (!end || end < start || start != 0x30000)
	{
		Console.WriteLn("CDVD: DVD structure rejected (start 0x%x, end 0x%x): not a DVD", start, end);
		return false;
	}
	if (l0->nlayers == 0)
	{
		m_media_type = 0;
		m_layer_break = 0;
		m_sectors = end - start + 1;
	}
	else if (l0->track_path == 0)
	{
		dvd_struct ds1 = {};
		ds1.format = DVD_STRUCT_PHYSICAL;
		ds1.layer_num = 1;
		if (ioctl(m_device, DVDIOCREADSTRUCTURE, &ds1) == -1)
			return false;
		const dvd_layer* l1 = reinterpret_cast<const dvd_layer*>(ds1.data);
		m_media_type = 1;
		m_layer_break = end - start;
		m_sectors = end - start + 1 + l1->end_sector - l1->start_sector + 1;
	}
	else
	{
		const u32 end_l0 = l0->end_sector_l0;
		m_media_type = 2;
		m_layer_break = end_l0 - start;
		m_sectors = end_l0 - start + 1 + end - (~end_l0 & 0xFFFFFFU) + 1;
	}
	return true;
}

bool IOCtlSrc::ReadCDInfo()
{
	ioc_toc_header th = {};
	if (ioctl(m_device, CDIOREADTOCHEADER, &th) == 0 && th.ending_track >= th.starting_track)
	{
		const int n = th.ending_track - th.starting_track + 2; // + lead-out
		std::vector<cd_toc_entry> entries(n);
		ioc_read_toc_entry te = {};
		te.address_format = CD_LBA_FORMAT;
		te.starting_track = th.starting_track;
		te.data_len = static_cast<u_short>(n * sizeof(cd_toc_entry));
		te.data = entries.data();
		if (ioctl(m_device, CDIOREADTOCENTRYS, &te) == 0)
		{
			// cd(4) hands LBA-format TOC addresses back in network byte order; an LBA beyond any disc means it didn't.
			const bool swap = !entries.empty() && ntohl(entries.back().addr.lba) < 0x1000000;
			for (const cd_toc_entry& e : entries)
			{
				const u32 lba = swap ? ntohl(e.addr.lba) : static_cast<u32>(e.addr.lba);
				if (e.track == 0xAA)
					m_sectors = lba;
				else if (e.track >= 1 && e.track < 100)
					m_toc.push_back({lba, e.track, static_cast<u8>(e.addr_type), static_cast<u8>(e.control)});
			}
			m_media_type = -1;
			if (m_sectors)
				return true;
		}
	}
	// No TOC: size the disc from the medium and treat it as one data track.
	off_t bytes = 0;
	if (ioctl(m_device, DIOCGMEDIASIZE, &bytes) == -1 || bytes <= 0)
		return false;
	m_sectors = static_cast<u32>(bytes / 2048);
	m_toc.clear();
	m_toc.push_back({0, 1, 1, 0x04});
	m_media_type = m_sectors > 400000 ? 0 : -1; // larger than any CD: a single-layer DVD
	return true;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
	(void)subq;
	return false;
}

bool IOCtlSrc::DiscReady()
{
	if (m_device == -1)
		return false;
	if (!m_sectors)
		Reopen(nullptr);
	return m_sectors != 0;
}

// The drive with a readable disc in it: the one named, else the PS5's own drive, else a USB drive.
static bool HasReadableDisc(const char* dev)
{
	const int fd = open(dev, O_RDONLY);
	if (fd == -1)
		return false;
	u8 sector[2048];
	const bool ok = pread(fd, sector, sizeof(sector), 16 * 2048) == static_cast<ssize_t>(sizeof(sector));
	close(fd);
	return ok;
}

std::vector<std::string> GetOpticalDriveList()
{
	std::vector<std::string> drives;
	for (int i = 0; i < 8; ++i)
	{
		const std::string dev = "/dev/cd" + std::to_string(i);
		struct stat st;
		if (stat(dev.c_str(), &st) == 0)
			drives.push_back(dev);
	}
	return drives;
}

void GetValidDrive(std::string& drive)
{
	if (!drive.empty() && drive.rfind("/dev/cd", 0) == 0 && HasReadableDisc(drive.c_str()))
		return;
	const std::string asked = drive;
	drive.clear();
	for (const std::string& dev : GetOpticalDriveList())
	{
		if (HasReadableDisc(dev.c_str()))
		{
			drive = dev;
			break;
		}
	}
	Console.WriteLn("CDVD: disc drive for '%s': %s", asked.c_str(), drive.empty() ? "none with a readable disc" : drive.c_str());
}
