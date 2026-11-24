// chiaki-ps5: PS Remote Play client for a jailbroken PS5 (streams another PS5).
// Video: FFmpeg (CPU) -> Vulkan (RADV on VideoOut) with picture filters and
// optional frame generation (ps5-framegen). Input: DualSense via scePad.
// Menu: hold OPTIONS + press D-pad UP. PS button: hold OPTIONS + press D-pad DOWN.

#include "vkfns.h"
#include "ps5_framegen.h"

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/discovery.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/base64.h>
#include <chiaki/orientation.h>
#include <chiaki/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <imgui.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "present_frag.h"
#include "fs_vert.h"

// ---------------------------------------------------------------- system APIs
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
int sceKernelDebugOutText(int channel, const char *text);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetInitialUser(int *user_id);
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, const void *param);
int scePadReadState(int handle, void *data);
int scePadSetVibration(int handle, const void *param);
int scePadSetMotionSensorState(int handle, bool enable);
int scePadSetLightBar(int handle, const void *param);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *ptr);
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addr);
}

struct PadTouch { uint16_t x, y; uint8_t id; uint8_t reserve[3]; };
struct PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
	uint8_t l2, r2;
	uint8_t pad0[2];
	float ox, oy, oz, ow;
	float ax, ay, az;
	float gx, gy, gz;
	uint8_t touchNum;
	uint8_t treserve[3];
	uint32_t treserve1;
	PadTouch touch[2];
	bool connected;
	uint64_t timestamp;
	uint8_t ext[24];
	uint8_t connectedCount;
	uint8_t reserve[2];
	uint8_t uniqueLen;
	uint8_t unique[12];
	uint8_t spare[256]; // room in case the PS5 writes a larger struct
};

enum : uint32_t
{
	PAD_CREATE = 0x00000001, PAD_L3 = 0x00000002, PAD_R3 = 0x00000004, PAD_OPTIONS = 0x00000008,
	PAD_UP = 0x00000010, PAD_RIGHT = 0x00000020, PAD_DOWN = 0x00000040, PAD_LEFT = 0x00000080,
	PAD_L2 = 0x00000100, PAD_R2 = 0x00000200, PAD_L1 = 0x00000400, PAD_R1 = 0x00000800,
	PAD_TRIANGLE = 0x00001000, PAD_CIRCLE = 0x00002000, PAD_CROSS = 0x00004000, PAD_SQUARE = 0x00008000,
	PAD_TOUCHPAD = 0x00100000,
};

// ---------------------------------------------------------------- Vulkan table
PFN_vkCreateInstance vkCreateInstance;
#define VKFN_DEFINE(n) PFN_vk##n vk##n;
VKFN_INSTANCE(VKFN_DEFINE)
VKFN_DEVICE(VKFN_DEFINE)
#undef VKFN_DEFINE

// ---------------------------------------------------------------- logging
#include <sys/param.h>
#include <sys/cpuset.h>
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_SIMD
#include "stb_image.h"

static const char *DATA_DIR = "/data/chiaki-ps5";
static std::mutex g_logMutex;
static FILE *g_logFile = nullptr;

static void AppLog(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> lock(g_logMutex);
	char k[1100];
	snprintf(k, sizeof(k), "[chiaki-ps5] %s\n", line);
	sceKernelDebugOutText(0, k);
	if(g_logFile) { fputs(k, g_logFile); fflush(g_logFile); }
}

static void AppChiakiLog(ChiakiLogLevel level, const char *msg, void *)
{
	AppLog("chiaki[%d]: %s", (int)level, msg);
}

static uint64_t NowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------- config / settings
struct Profile { std::string name, account_b64; };
struct Config { std::string host; std::vector<Profile> profiles; };

static std::string ReadFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	if(!f) return {};
	std::string s;
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}

// Tiny JSON helpers: "key": "value" lookups starting at pos.
static std::string JsonStr(const std::string &j, const char *key, size_t from = 0, size_t *end = nullptr)
{
	std::string k = std::string("\"") + key + "\"";
	size_t p = j.find(k, from);
	if(p == std::string::npos) return {};
	p = j.find(':', p + k.size());
	if(p == std::string::npos) return {};
	p = j.find('"', p);
	if(p == std::string::npos) return {};
	size_t q = j.find('"', p + 1);
	if(q == std::string::npos) return {};
	if(end) *end = q + 1;
	return j.substr(p + 1, q - p - 1);
}

static Config LoadConfig()
{
	Config c;
	std::string j = ReadFile((std::string(DATA_DIR) + "/config.json").c_str());
	c.host = JsonStr(j, "ps5_ip");
	size_t pos = j.find("\"profiles\"");
	while(pos != std::string::npos)
	{
		size_t e1 = 0, e2 = 0;
		std::string name = JsonStr(j, "name", pos, &e1);
		if(name.empty()) break;
		std::string acc = JsonStr(j, "psn_account_id", e1, &e2);
		if(acc.empty()) break;
		c.profiles.push_back({name, acc});
		pos = e2;
	}
	return c;
}

static std::string JsonEsc(const std::string &in)
{
	std::string o;
	for(char c : in)
	{
		if(c == '"' || c == '\\') o += '\\';
		if((unsigned char)c >= 32) o += c;
	}
	return o;
}

static bool SaveConfig(const Config &c)
{
	std::string path = std::string(DATA_DIR) + "/config.json", tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if(!f) return false;
	fprintf(f, "{\n  \"ps5_ip\": \"%s\",\n  \"profiles\": [\n", JsonEsc(c.host).c_str());
	for(size_t i = 0; i < c.profiles.size(); i++)
		fprintf(f, "    { \"name\": \"%s\", \"psn_account_id\": \"%s\" }%s\n", JsonEsc(c.profiles[i].name).c_str(),
			JsonEsc(c.profiles[i].account_b64).c_str(), i + 1 < c.profiles.size() ? "," : "");
	fprintf(f, "  ]\n}\n");
	fclose(f);
	remove(path.c_str());
	return rename(tmp.c_str(), path.c_str()) == 0;
}

// An account ID as typed: base64 of the 8 bytes (what chiaki uses), or the decimal number. Gives the base64 form.
static bool NormalizeAccount(const std::string &in, std::string &out)
{
	std::string t;
	for(char c : in) if(c != ' ') t += c;
	if(t.empty()) return false;
	bool digits = true;
	for(char c : t) if(c < '0' || c > '9') digits = false;
	uint8_t b[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	if(digits)
	{
		if(t.size() > 20) return false;
		unsigned long long v = strtoull(t.c_str(), nullptr, 10);
		if(!v) return false;
		for(int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
	}
	else
	{
		size_t n = sizeof(b);
		if(chiaki_base64_decode(t.c_str(), t.size(), b, &n) != CHIAKI_ERR_SUCCESS || n != sizeof(b)) return false;
	}
	char enc[32] = {0};
	if(chiaki_base64_encode(b, sizeof(b), enc, sizeof(enc)) != CHIAKI_ERR_SUCCESS) return false;
	out = enc;
	return true;
}

struct Settings
{
	int restore = 1;   // 0 off, 1 low, 2 high
	int deband = 1;    // 0/1
	int sharpen = 4;   // 0..10
	int boost = 1;     // 0 off, 1 low, 2 high
	int framegen = 0;  // 0 off, 1 30->60, 2 60->120
	int fps = 0;       // show fps counter
	int res720 = 0;    // old setting (0 1080p, 1 720p), read for compatibility
	int res = 1;       // 0 720p, 1 1080p, 2 1440p, 3 4K
	int decmode = 0;   // decoder: 0 lowest latency, 1 fast (2 frame threads), 2 fastest (4 frame threads)
	int bitrate = 3;   // index into kBitrates
	int h264 = 0;      // 0 HEVC, 1 H.264
	int hz120 = 0;     // 1: use the 1080p 120 Hz output mode (restart app)
};
static const int kBitrates[] = {10000, 15000, 20000, 30000, 40000, 50000};
static const int kBitrateCount = 6;

static std::string SettingsPath() { return std::string(DATA_DIR) + "/settings.txt"; }

static void SaveSettings(const Settings &s)
{
	FILE *f = fopen(SettingsPath().c_str(), "w");
	if(!f) return;
	fprintf(f, "restore=%d\ndeband=%d\nsharpen=%d\nboost=%d\nframegen=%d\nfps=%d\nres=%d\nbitrate=%d\nh264=%d\nhz120=%d\ndecmode=%d\n",
		s.restore, s.deband, s.sharpen, s.boost, s.framegen, s.fps, s.res, s.bitrate, s.h264, s.hz120, s.decmode);
	fclose(f);
}

static Settings LoadSettings()
{
	Settings s;
	std::string t = ReadFile(SettingsPath().c_str());
	auto get = [&](const char *k, int &v, int lo, int hi) {
		std::string key = std::string(k) + "=";
		size_t p = t.find(key);
		if(p == std::string::npos) return;
		int x = atoi(t.c_str() + p + key.size());
		if(x >= lo && x <= hi) v = x;
	};
	get("restore", s.restore, 0, 2);
	get("deband", s.deband, 0, 1);
	get("sharpen", s.sharpen, 0, 10);
	get("boost", s.boost, 0, 2);
	get("framegen", s.framegen, 0, 2);
	get("fps", s.fps, 0, 1);
	get("res720", s.res720, 0, 1);
	get("bitrate", s.bitrate, 0, kBitrateCount - 1);
	get("h264", s.h264, 0, 1);
	get("hz120", s.hz120, 0, 1);
	s.res = s.res720 ? 0 : 1;
	get("res", s.res, 0, 3);
	get("decmode", s.decmode, 0, 2);
	return s;
}

static std::string HostPath(const std::string &profile)
{
	std::string safe;
	for(char c : profile) safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	return std::string(DATA_DIR) + "/registered_host_" + safe + ".bin";
}

static bool LoadHost(const std::string &profile, ChiakiRegisteredHost *h)
{
	FILE *f = fopen(HostPath(profile).c_str(), "rb");
	if(!f) return false;
	bool ok = fread(h, 1, sizeof(*h), f) == sizeof(*h);
	fclose(f);
	return ok;
}

static void SaveHost(const std::string &profile, const ChiakiRegisteredHost *h)
{
	FILE *f = fopen(HostPath(profile).c_str(), "wb");
	if(!f) return;
	fwrite(h, 1, sizeof(*h), f);
	fclose(f);
}

// ---------------------------------------------------------------- UI overlay (CPU, 960x540 RGBA)
static const int UI_W = 1920, UI_H = 1080; // layout below is written in 960x540 units (U = 2 px)
static const int U = 2;
static std::vector<uint32_t> g_ui(UI_W *UI_H);
static ImFontAtlas *g_atlas;
static ImFont *g_fonts[4];
static unsigned char *g_fontPx;
static int g_fontW, g_fontH;

static uint32_t RGBA(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }

static void FontInit()
{
	g_atlas = new ImFontAtlas();
	const char *path = "/preinst/common/font/SST-Medium.otf";
	const float sizes[4] = {26.0f, 30.0f, 52.0f, 76.0f}; // index = text scale
	FILE *f = fopen(path, "rb");
	bool have = f != nullptr;
	if(f) fclose(f);
	for(int i = 0; i < 4; i++)
	{
		ImFont *ft = nullptr;
		if(have)
		{
			ImFontConfig cfg;
			cfg.OversampleH = 1; // glyphs are copied 1:1, so no oversampling
			cfg.OversampleV = 1;
			cfg.PixelSnapH = true;
			ft = g_atlas->AddFontFromFileTTF(path, sizes[i], &cfg);
		}
		if(!ft)
		{
			ImFontConfig cfg;
			cfg.SizePixels = 13.0f * (i == 0 ? 2 : i * 2);
			ft = g_atlas->AddFontDefault(&cfg);
		}
		g_fonts[i] = ft;
	}
	g_atlas->GetTexDataAsAlpha8(&g_fontPx, &g_fontW, &g_fontH);
	AppLog("UI font: %s", have ? path : "built-in");
}
static ImFont *FontFor(int scale) { return g_fonts[scale < 1 ? 1 : scale > 3 ? 3 : scale]; }

static void UiClear() { std::fill(g_ui.begin(), g_ui.end(), 0u); }

static void Blend(int x, int y, uint32_t c, int a)
{
	if(x < 0 || y < 0 || x >= UI_W || y >= UI_H || a <= 0) return;
	uint32_t &d = g_ui[y * UI_W + x];
	int sa = (int)((c >> 24) * a / 255);
	int da = d >> 24;
	int oa = sa + da * (255 - sa) / 255;
	if(oa == 0) { d = 0; return; }
	int out[3];
	for(int i = 0; i < 3; i++)
	{
		int sc = (c >> (8 * i)) & 0xff, dc = (d >> (8 * i)) & 0xff;
		out[i] = (sc * sa + dc * da * (255 - sa) / 255) / oa;
	}
	d = RGBA(out[0], out[1], out[2], oa);
}

// ---- chiaki-ng look (Material dark, accent #00a7ff), drawn in 960x540 units
struct UiImg { int w = 0, h = 0; std::vector<uint8_t> px; };
static UiImg g_imgConsole, g_imgLogo;

static void UiRoundRect(int x, int y, int w, int h, int r, uint32_t c)
{
	int x0 = x * U, y0 = y * U, x1 = (x + w) * U, y1 = (y + h) * U, rr = r * U;
	for(int yy = y0; yy < y1; yy++)
		for(int xx = x0; xx < x1; xx++)
		{
			int cx = xx < x0 + rr ? x0 + rr : (xx >= x1 - rr ? x1 - rr - 1 : xx);
			int cy = yy < y0 + rr ? y0 + rr : (yy >= y1 - rr ? y1 - rr - 1 : yy);
			int a = 255;
			if(cx != xx || cy != yy)
			{
				float dx = (float)(xx - cx), dy = (float)(yy - cy);
				float d = sqrtf(dx * dx + dy * dy) - (float)rr;
				if(d > 0.5f) continue;
				if(d > -0.5f) a = (int)((0.5f - d) * 255.0f);
			}
			Blend(xx, yy, c, a);
		}
}

static void UiImage(const UiImg &img, int x, int y, float opacity)
{
	if(img.px.empty()) return;
	int bx = x * U, by = y * U, o = (int)(opacity * 255.0f);
	for(int yy = 0; yy < img.h; yy++)
		for(int xx = 0; xx < img.w; xx++)
		{
			const uint8_t *p = &img.px[(yy * img.w + xx) * 4];
			if(p[3]) Blend(bx + xx, by + yy, RGBA(p[0], p[1], p[2], 255), p[3] * o / 255);
		}
}

static void UiRect(int x, int y, int w, int h, uint32_t c)
{
	for(int yy = y * U; yy < (y + h) * U; yy++)
		for(int xx = x * U; xx < (x + w) * U; xx++)
			Blend(xx, yy, c, 255);
}

static int UiTextWidth(const char *s, int scale)
{
	ImFont *ft = FontFor(scale);
	float w = 0;
	for(; *s; s++)
	{
		const ImFontGlyph *g = ft->FindGlyph((ImWchar)(unsigned char)*s);
		if(g) w += g->AdvanceX;
	}
	return (int)(w / U);
}

static void UiText(int x, int y, const char *s, uint32_t c, int scale = 1)
{
	ImFont *ft = FontFor(scale);
	float cx = (float)(x * U);
	int baseY = y * U;
	for(; *s; s++)
	{
		const ImFontGlyph *g = ft->FindGlyph((ImWchar)(unsigned char)*s);
		if(!g) continue;
		int u0 = (int)(g->U0 * g_fontW + 0.5f), v0 = (int)(g->V0 * g_fontH + 0.5f);
		int u1 = (int)(g->U1 * g_fontW + 0.5f), v1 = (int)(g->V1 * g_fontH + 0.5f);
		int gx = (int)(cx + g->X0), gy = (int)(baseY + g->Y0);
		for(int yy = 0; yy < v1 - v0; yy++)
			for(int xx = 0; xx < u1 - u0; xx++)
				Blend(gx + xx, gy + yy, c, g_fontPx[(v0 + yy) * g_fontW + u0 + xx]);
		cx += g->AdvanceX;
	}
}

static void UiTextCentered(int y, const char *s, uint32_t c, int scale = 1)
{
	UiText((UI_W / U - UiTextWidth(s, scale)) / 2, y, s, c, scale);
}

// Word-wrap a long status line into the panel.
static void UiTextWrapped(int y, const std::string &s, uint32_t c, int scale, int maxw)
{
	std::string line, word;
	auto flush = [&]() {
		if(!line.empty()) { UiTextCentered(y, line.c_str(), c, scale); y += 16 * scale; line.clear(); }
	};
	for(size_t i = 0; i <= s.size(); i++)
	{
		char ch = i < s.size() ? s[i] : ' ';
		if(ch == ' ')
		{
			std::string trial = line.empty() ? word : line + " " + word;
			if(UiTextWidth(trial.c_str(), scale) > maxw) { flush(); line = word; }
			else line = trial;
			word.clear();
		}
		else word += ch;
	}
	flush();
}

// ---------------------------------------------------------------- Vulkan renderer
struct Img
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	uint32_t w = 0, h = 0;
};
struct Buf
{
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	void *map = nullptr;
	VkDeviceSize size = 0;
};

static const uint32_t FRAME_W = 1920, FRAME_H = 1080;
static const uint32_t VIDEO_MAX_W = 3840, VIDEO_MAX_H = 2160; // largest stream the staging buffers hold
static const VkFormat FRAME_FMT = VK_FORMAT_R8G8B8A8_UNORM;

struct PushConstants
{
	float rect[4];
	float sharpen, deband, restore, boost;
	float hasVideo, uiOn, time, pad;
};

class Renderer
{
public:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memProps{};
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
	VkExtent2D extent{1920, 1080};
	std::vector<VkImage> swapImages;
	double displayHz = 60.0;
	bool want120 = false;
	bool splashHidden = false;

	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmds[2]{};
	VkFence fences[2]{};
	VkSemaphore acquireSem[2]{};
	std::vector<VkSemaphore> doneSem;
	int slot = 0;

	Img frame;              // filtered frame, RGBA8 1920x1080 (GENERAL)
	Img texY, texU, texV;   // video planes
	Img texUI;
	Buf staging[2];         // per slot: video planes + UI
	VkSampler linear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkDescriptorPool descPool = VK_NULL_HANDLE;
	VkDescriptorSet descSet = VK_NULL_HANDLE;

	ps5::framegen::interpolator fg;
	bool fgReady = false;

	uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want)
	{
		for(uint32_t i = 0; i < memProps.memoryTypeCount; i++)
			if((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return i;
		return UINT32_MAX;
	}

	bool MakeImage(Img &img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage)
	{
		VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ci.imageType = VK_IMAGE_TYPE_2D;
		ci.format = fmt;
		ci.extent = {w, h, 1};
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.samples = VK_SAMPLE_COUNT_1_BIT;
		ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		ci.usage = usage;
		ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if(vkCreateImage(device, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(device, img.image, &req);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if(ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = MemType(req.memoryTypeBits, 0);
		if(vkAllocateMemory(device, &ai, nullptr, &img.mem) != VK_SUCCESS) return false;
		vkBindImageMemory(device, img.image, img.mem, 0);
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = img.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if(vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
		img.w = w;
		img.h = h;
		return true;
	}

	void FreeImage(Img &img)
	{
		if(img.view) vkDestroyImageView(device, img.view, nullptr);
		if(img.image) vkDestroyImage(device, img.image, nullptr);
		if(img.mem) vkFreeMemory(device, img.mem, nullptr);
		img = Img{};
	}

	bool MakeBuffer(Buf &b, VkDeviceSize size)
	{
		VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		ci.size = size;
		ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if(vkCreateBuffer(device, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, b.buf, &req);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if(ai.memoryTypeIndex == UINT32_MAX) return false;
		if(vkAllocateMemory(device, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
		vkBindBufferMemory(device, b.buf, b.mem, 0);
		if(vkMapMemory(device, b.mem, 0, size, 0, &b.map) != VK_SUCCESS) return false;
		b.size = size;
		return true;
	}

	static void Barrier(VkCommandBuffer cmd)
	{
		VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	}

	static void Layout(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to)
	{
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	bool Init()
	{
		vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
		if(!vkCreateInstance) { AppLog("no vkCreateInstance"); return false; }
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "chiaki-ps5";
		app.apiVersion = VK_API_VERSION_1_3;
		const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = iext;
		VkResult r = vkCreateInstance(&ici, nullptr, &instance);
		if(r != VK_SUCCESS) { AppLog("vkCreateInstance failed %d", r); return false; }
		bool ok = true;
#define LOAD_I(n) vk##n = (PFN_vk##n)vk_icdGetInstanceProcAddr(instance, "vk" #n); if(!vk##n) { AppLog("missing vk" #n); ok = false; }
		VKFN_INSTANCE(LOAD_I)
#undef LOAD_I
		if(!ok) return false;

		uint32_t count = 1;
		vkEnumeratePhysicalDevices(instance, &count, &physical);
		if(count == 0) { AppLog("no GPU"); return false; }
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physical, &props);
		AppLog("GPU: %s", props.deviceName);
		vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

		VkQueueFamilyProperties fams[8];
		count = 8;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, fams);
		for(uint32_t i = 0; i < count; i++)
			AppLog("queue family %u: flags 0x%x, %u queues (video decode %s)", i, fams[i].queueFlags, fams[i].queueCount,
				(fams[i].queueFlags & 0x20) ? "YES" : "no");
		{
			uint32_t ne = 0;
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
			std::vector<VkExtensionProperties> ex(ne);
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, ex.data());
			std::string vids;
			for(auto &e : ex) if(strstr(e.extensionName, "video")) { vids += e.extensionName; vids += ' '; }
			AppLog("device extensions: %u, video ones: %s", ne, vids.empty() ? "(none)" : vids.c_str());
		}
		family = UINT32_MAX;
		for(uint32_t i = 0; i < count; i++)
			if((fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
		if(family == UINT32_MAX) { AppLog("no graphics queue"); return false; }

		float prio = 1.0f;
		VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = family;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;
		VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		f13.dynamicRendering = VK_TRUE;
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		f2.pNext = &f13;
		f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
		const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.pNext = &f2;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qi;
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = dext;
		r = vkCreateDevice(physical, &dci, nullptr, &device);
		if(r != VK_SUCCESS) { AppLog("vkCreateDevice failed %d", r); return false; }
#define LOAD_D(n) vk##n = (PFN_vk##n)vkGetDeviceProcAddr(device, "vk" #n); if(!vk##n) vk##n = (PFN_vk##n)vk_icdGetInstanceProcAddr(instance, "vk" #n); if(!vk##n) { AppLog("missing vk" #n); ok = false; }
		VKFN_DEVICE(LOAD_D)
#undef LOAD_D
		if(!ok) return false;
		vkGetDeviceQueue(device, family, 0, &queue);

		if(!CreateDisplay()) return false;
		if(!CreateResources()) return false;

		std::string err;
		fgReady = fg.create(physical, device, FRAME_W, FRAME_H, err, FRAME_FMT);
		AppLog("frame generation: %s %s", fgReady ? "ready" : "unavailable", err.c_str());
		return true;
	}

	bool CreateDisplay()
	{
		VkDisplayPropertiesKHR disp;
		uint32_t count = 1;
		VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &disp);
		if(count == 0) { AppLog("no display (%d)", r); return false; }
		VkDisplayModePropertiesKHR modes[16];
		uint32_t mc = 16;
		vkGetDisplayModePropertiesKHR(physical, disp.display, &mc, modes);
		if(mc == 0) { AppLog("no display modes"); return false; }
		int best = -1;
		for(uint32_t i = 0; i < mc; i++)
		{
			const auto &mp = modes[i].parameters;
			AppLog("display mode %u: %ux%u @ %.3f Hz", i, mp.visibleRegion.width, mp.visibleRegion.height, mp.refreshRate / 1000.0);
			bool hz120 = mp.refreshRate > 100000;
			if(mp.visibleRegion.width == 1920 && mp.visibleRegion.height == 1080 && hz120 == want120)
				best = (int)i;
		}
		if(best < 0)
			for(uint32_t i = 0; i < mc; i++)
				if(modes[i].parameters.visibleRegion.width == 1920) { best = (int)i; break; }
		if(best < 0) best = 0;
		displayHz = modes[best].parameters.refreshRate / 1000.0;
		AppLog("display: %s, using mode %d (%.3f Hz)", disp.displayName ? disp.displayName : "?", best, displayHz);

		VkDisplaySurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
		si.displayMode = modes[best].displayMode;
		si.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		si.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
		si.imageExtent = modes[best].parameters.visibleRegion;
		r = vkCreateDisplayPlaneSurfaceKHR(instance, &si, nullptr, &surface);
		if(r != VK_SUCCESS) { AppLog("display surface failed %d", r); return false; }
		VkBool32 sup = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &sup);
		VkSurfaceCapabilitiesKHR caps;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
		extent = caps.currentExtent.width ? caps.currentExtent : si.imageExtent;
		AppLog("surface %ux%u, usage 0x%x, images %u..%u", extent.width, extent.height, caps.supportedUsageFlags, caps.minImageCount, caps.maxImageCount);
		if(!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) AppLog("WARNING: swapchain has no TRANSFER_DST usage");

		VkSurfaceFormatKHR fmts[16];
		uint32_t fc = 16;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fc, fmts);
		swapFormat = fc ? fmts[0].format : VK_FORMAT_B8G8R8A8_UNORM;
		VkColorSpaceKHR cs = fc ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		for(uint32_t i = 0; i < fc; i++)
			if(fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { swapFormat = fmts[i].format; cs = fmts[i].colorSpace; break; }
		AppLog("swapchain format %d", swapFormat);

		uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount;
		if(caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
		VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		sc.surface = surface;
		sc.minImageCount = n;
		sc.imageFormat = swapFormat;
		sc.imageColorSpace = cs;
		sc.imageExtent = extent;
		sc.imageArrayLayers = 1;
		sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		sc.clipped = VK_TRUE;
		r = vkCreateSwapchainKHR(device, &sc, nullptr, &swapchain);
		if(r != VK_SUCCESS) { AppLog("swapchain failed %d", r); return false; }
		uint32_t ic = 0;
		vkGetSwapchainImagesKHR(device, swapchain, &ic, nullptr);
		swapImages.resize(ic);
		vkGetSwapchainImagesKHR(device, swapchain, &ic, swapImages.data());
		AppLog("swapchain: %u images", ic);
		return true;
	}

	VkShaderModule Module(const uint32_t *code, size_t bytes)
	{
		VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(device, &ci, nullptr, &m);
		return m;
	}

	bool CreateResources()
	{
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = family;
		vkCreateCommandPool(device, &pci, nullptr, &pool);
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 2;
		vkAllocateCommandBuffers(device, &cai, cmds);
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for(int i = 0; i < 2; i++)
		{
			vkCreateFence(device, &fci, nullptr, &fences[i]);
			vkCreateSemaphore(device, &sci, nullptr, &acquireSem[i]);
		}
		doneSem.resize(swapImages.size());
		for(auto &s : doneSem) vkCreateSemaphore(device, &sci, nullptr, &s);

		if(!MakeImage(frame, FRAME_W, FRAME_H, FRAME_FMT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
		if(!MakeImage(texUI, UI_W, UI_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
		if(!MakeVideoTextures(16, 16)) return false;
		VkDeviceSize stSize = (VkDeviceSize)VIDEO_MAX_W * VIDEO_MAX_H * 3 / 2 + (VkDeviceSize)UI_W * UI_H * 4 + 4096;
		for(auto &s : staging) if(!MakeBuffer(s, stSize)) { AppLog("staging alloc failed"); return false; }

		VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
		smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		smp.maxLod = 0.0f;
		vkCreateSampler(device, &smp, nullptr, &linear);
		smp.magFilter = smp.minFilter = VK_FILTER_NEAREST;
		vkCreateSampler(device, &smp, nullptr, &nearest);

		VkDescriptorSetLayoutBinding binds[4];
		for(int i = 0; i < 4; i++)
		{
			binds[i] = {};
			binds[i].binding = i;
			binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			binds[i].descriptorCount = 1;
			binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dl.bindingCount = 4;
		dl.pBindings = binds;
		vkCreateDescriptorSetLayout(device, &dl, nullptr, &setLayout);
		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
		VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.setLayoutCount = 1;
		pl.pSetLayouts = &setLayout;
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &pcr;
		vkCreatePipelineLayout(device, &pl, nullptr, &pipeLayout);

		VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
		VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dp.maxSets = 1;
		dp.poolSizeCount = 1;
		dp.pPoolSizes = &ps;
		vkCreateDescriptorPool(device, &dp, nullptr, &descPool);
		VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		da.descriptorPool = descPool;
		da.descriptorSetCount = 1;
		da.pSetLayouts = &setLayout;
		vkAllocateDescriptorSets(device, &da, &descSet);
		UpdateDescriptors();

		VkShaderModule vs = Module(fs_vert, sizeof(fs_vert));
		VkShaderModule fsm = Module(present_frag, sizeof(present_frag));
		VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fsm;
		stages[1].pName = "main";
		VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &cba;
		VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates = dyn;
		VkFormat colorFmt = FRAME_FMT;
		VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		pr.colorAttachmentCount = 1;
		pr.pColorAttachmentFormats = &colorFmt;
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.pNext = &pr;
		gp.stageCount = 2;
		gp.pStages = stages;
		gp.pVertexInputState = &vin;
		gp.pInputAssemblyState = &ia;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &rs;
		gp.pMultisampleState = &ms;
		gp.pColorBlendState = &cb;
		gp.pDynamicState = &ds;
		gp.layout = pipeLayout;
		VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
		vkDestroyShaderModule(device, vs, nullptr);
		vkDestroyShaderModule(device, fsm, nullptr);
		if(r != VK_SUCCESS) { AppLog("pipeline failed %d", r); return false; }

		// initial layouts
		VkCommandBuffer cmd = cmds[0];
		vkWaitForFences(device, 1, &fences[0], VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &fences[0]);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(cmd, &bi);
		Layout(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		Layout(cmd, texUI.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		InitVideoLayouts(cmd);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		vkQueueSubmit(queue, 1, &si, fences[0]);
		vkWaitForFences(device, 1, &fences[0], VK_TRUE, UINT64_MAX);
		return true;
	}

	bool videoLayoutPending = false;
	bool MakeVideoTextures(uint32_t w, uint32_t h)
	{
		FreeImage(texY);
		FreeImage(texU);
		FreeImage(texV);
		VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if(!MakeImage(texY, w, h, VK_FORMAT_R8_UNORM, u)) return false;
		if(!MakeImage(texU, w / 2, h / 2, VK_FORMAT_R8_UNORM, u)) return false;
		if(!MakeImage(texV, w / 2, h / 2, VK_FORMAT_R8_UNORM, u)) return false;
		videoLayoutPending = true;
		return true;
	}

	void InitVideoLayouts(VkCommandBuffer cmd)
	{
		if(!videoLayoutPending) return;
		Layout(cmd, texY.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Layout(cmd, texU.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		Layout(cmd, texV.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		videoLayoutPending = false;
	}

	void UpdateDescriptors()
	{
		VkDescriptorImageInfo ii[4] = {
			{linear, texY.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{linear, texU.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{linear, texV.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{linear, texUI.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		};
		VkWriteDescriptorSet w[4];
		for(int i = 0; i < 4; i++)
		{
			w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w[i].dstSet = descSet;
			w[i].dstBinding = i;
			w[i].descriptorCount = 1;
			w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
	}

	// Resize the video textures (between frames, GPU idle).
	void EnsureVideoSize(uint32_t w, uint32_t h)
	{
		if(texY.w == w && texY.h == h) return;
		vkDeviceWaitIdle(device);
		MakeVideoTextures(w, h);
		UpdateDescriptors();
		AppLog("video size %ux%u", w, h);
	}

	void Upload(VkCommandBuffer cmd, Img &img, VkDeviceSize offset)
	{
		Layout(cmd, img.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy c{};
		c.bufferOffset = offset;
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {img.w, img.h, 1};
		vkCmdCopyBufferToImage(cmd, staging[slot].buf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		Layout(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	VkDeviceSize UiOffset() const { return (VkDeviceSize)VIDEO_MAX_W * VIDEO_MAX_H * 3 / 2 + 256; }

	// Begin the slot's command buffer (waits for its previous use).
	VkCommandBuffer Begin()
	{
		vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
		VkCommandBuffer cmd = cmds[slot];
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
		InitVideoLayouts(cmd);
		Barrier(cmd);
		return cmd;
	}

	void Draw(VkCommandBuffer cmd, bool uploadVideo, bool uploadUi, const PushConstants &pc)
	{
		if(uploadVideo)
		{
			VkDeviceSize ys = (VkDeviceSize)texY.w * texY.h, cs = (VkDeviceSize)texU.w * texU.h;
			Upload(cmd, texY, 0);
			Upload(cmd, texU, ys);
			Upload(cmd, texV, ys + cs);
		}
		if(uploadUi) Upload(cmd, texUI, UiOffset());
		Barrier(cmd);
		VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		att.imageView = frame.view;
		att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {FRAME_W, FRAME_H}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &att;
		vkCmdBeginRendering(cmd, &ri);
		VkViewport v{0, 0, (float)FRAME_W, (float)FRAME_H, 0, 1};
		VkRect2D sc{{0, 0}, {FRAME_W, FRAME_H}};
		vkCmdSetViewport(cmd, 0, 1, &v);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &descSet, 0, nullptr);
		vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);
		Barrier(cmd);
	}

	// Copy the frame into the frame generator's current input.
	void CopyIntoFg(VkCommandBuffer cmd)
	{
		fg.prepare(cmd);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		b.srcOffsets[1] = b.dstOffsets[1] = {(int32_t)FRAME_W, (int32_t)FRAME_H, 1};
		vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_GENERAL, fg.frame_image(), VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
		Barrier(cmd);
	}

	// Blit src to the next swapchain image, submit, present.
	bool EndAndPresent(VkCommandBuffer cmd, VkImage src)
	{
		uint32_t idx = 0;
		VkResult r = vkAcquireNextImageKHR(device, swapchain, 1000000000ull, acquireSem[slot], VK_NULL_HANDLE, &idx);
		if(r < 0) { AppLog("acquire failed %d", r); vkEndCommandBuffer(cmd); return false; }
		VkImage dst = swapImages[idx];
		Layout(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		b.srcOffsets[1] = {(int32_t)FRAME_W, (int32_t)FRAME_H, 1};
		b.dstOffsets[1] = {(int32_t)extent.width, (int32_t)extent.height, 1};
		vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
			(extent.width == FRAME_W && extent.height == FRAME_H) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		Layout(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		vkEndCommandBuffer(cmd);
		VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &acquireSem[slot];
		si.pWaitDstStageMask = &wait;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &doneSem[idx];
		vkResetFences(device, 1, &fences[slot]);
		r = vkQueueSubmit(queue, 1, &si, fences[slot]);
		if(r != VK_SUCCESS) { AppLog("submit failed %d", r); return false; }
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &doneSem[idx];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &idx;
		r = vkQueuePresentKHR(queue, &pi);
		if(r < 0) AppLog("present failed %d", r);
		if(!splashHidden && r >= 0)
		{
			splashHidden = true;
			// Resolved at run time so the title imports exactly the modules the first
			// (launchable) build did.
			int mod = sceKernelLoadStartModule("/system/common/lib/libSceSystemService.sprx", 0, nullptr, 0, nullptr, nullptr);
			void *fn = nullptr;
			if(mod >= 0) sceKernelDlsym(mod, "sceSystemServiceHideSplashScreen", &fn);
			int hr = fn ? ((int (*)(void))fn)() : -1;
			AppLog("first frame presented; libSceSystemService %d, hide splash: %d", mod, hr);
		}
		slot ^= 1;
		return true;
	}
};

// ---------------------------------------------------------------- the app
enum class State { ProfileSelect, PinEntry, Registering, Connecting, Streaming, Error, EditProfile, Keyboard, Help };

class App
{
public:
	Renderer gfx;
	Config cfg;
	Settings set;
	ChiakiLog log;
	int userId = 0;
	int pad = -1;

	std::atomic<State> state{State::ProfileSelect};
	std::mutex statusMutex;
	std::string statusMsg;
	void Status(const std::string &s) { std::lock_guard<std::mutex> l(statusMutex); statusMsg = s; uiDirty = true; }
	std::string GetStatus() { std::lock_guard<std::mutex> l(statusMutex); return statusMsg; }

	int profileIdx = 0;
	// profile editor
	int editIdx = -1;          // -1: a new profile
	int editSel = 0;
	std::string editName, editAcc, editMsg;
	// on-screen keyboard
	std::string kbBuf, kbTitle;
	std::string *kbTarget = nullptr;
	int kbMode = 0;            // 0 text, 1 IP address
	int kbRow = 1, kbCol = 0;
	bool kbShift = false;
	size_t kbMax = 16;
	State kbBack = State::ProfileSelect;
	std::string profileName;
	uint8_t accountId[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	int pinDigits[8] = {0};
	int pinCursor = 0;

	ChiakiRegist regist;
	bool registActive = false;
	std::atomic<bool> registDone{false}, wantConnect{false};
	ChiakiRegisteredHost host;

	ChiakiSession session;
	ChiakiOpusDecoder opus;
	bool sessionActive = false;
	std::atomic<bool> sessionQuit{false};

	// controller
	ChiakiOrientationTracker orient;
	ChiakiAccelNewZero accelZero;
	uint32_t prevButtons = 0;
	uint64_t comboStart = 0;
	bool comboFired = false;
	bool menuOpen = false;
	uint64_t psUntil = 0;      // send PS until this time (us)
	bool holdUntilRelease = false; // after the menu closes, ignore buttons until all are released
	uint64_t stickComboStart = 0;
	bool stickComboFired = false;
	bool optionsHeld = false, optionsUsed = false;
	uint64_t optionsTapUntil = 0;
	int menuIdx = 0;
	std::atomic<uint8_t> rumbleL{0}, rumbleR{0};
	std::atomic<bool> rumbleDirty{false};

	// video
	AVCodecContext *dec = nullptr;
	AVFrame *avf = nullptr;
	AVPacket *pkt = nullptr;
	std::mutex frameMutex;
	std::condition_variable frameCv;
	std::vector<uint8_t> frameBuf;
	uint32_t frameW = 0, frameH = 0;
	uint64_t frameSeq = 0, shownSeq = 0;
	bool haveVideo = false;
	bool fgReset = true;
	bool streamIs30 = false;

	// compressed video handed from chiaki's receive thread to our decode thread
	std::mutex pktMutex;
	std::condition_variable pktCv;
	std::vector<std::vector<uint8_t>> pktQueue;
	std::atomic<bool> decodeRun{false};
	std::thread decodeThread;
	uint64_t decodeSlowLogged = 0;

	void StartDecodeThread()
	{
		StopDecodeThread();
		decodeRun = true;
		decodeThread = std::thread([this]() {
			std::vector<uint8_t> p;
			while(decodeRun)
			{
				{
					std::unique_lock<std::mutex> l(pktMutex);
					pktCv.wait_for(l, std::chrono::milliseconds(50), [this] { return !pktQueue.empty() || !decodeRun; });
					if(!decodeRun) break;
					if(pktQueue.empty()) continue;
					p.swap(pktQueue.front());
					pktQueue.erase(pktQueue.begin());
				}
				uint64_t t0 = NowUs();
				{
					std::lock_guard<std::mutex> dl(decMutex);
					Decode(p.data(), p.size());
				}
				uint64_t dt = NowUs() - t0;
				decSum += dt; decN++; if(dt > decMax) decMax = dt;
				if(NowUs() - decodeSlowLogged > 5000000)
				{
					decodeSlowLogged = NowUs();
					AppLog("decode: %d frames, avg %.1f ms, max %.1f ms, %ux%u (queue %d)", decN, decN ? decSum / 1000.0 / decN : 0.0,
						decMax / 1000.0, texWLog, texHLog, (int)pktQueue.size());
					decSum = 0; decN = 0; decMax = 0;
				}
			}
		});
	}

	void StopDecodeThread()
	{
		decodeRun = false;
		pktCv.notify_all();
		if(decodeThread.joinable()) decodeThread.join();
		std::lock_guard<std::mutex> l(pktMutex);
		pktQueue.clear();
	}

	void QueuePacket(uint8_t *buf, size_t size)
	{
		std::lock_guard<std::mutex> l(pktMutex);
		if(pktQueue.size() > 30) { pktQueue.clear(); AppLog("decode queue overflow, dropped"); }
		pktQueue.emplace_back(buf, buf + size);
		pktCv.notify_one();
	}
	std::mutex decMutex;

	// audio
	std::mutex audioMutex;
	std::vector<int16_t> audioRing;
	size_t audioRead = 0, audioWrite = 0, audioFill = 0;
	int audioPort = -1;
	std::atomic<bool> running{true};

	// fps
	uint64_t fpsT0 = 0;
	int fpsFrames = 0, fpsPresents = 0;
	int fpsShown = 0, fpsShownOut = 0;

	bool uiDirty = true;

	bool Init()
	{
		mkdir(DATA_DIR, 0777);
		g_logFile = fopen((std::string(DATA_DIR) + "/log.txt").c_str(), "w");
		AppLog("chiaki4ps5 starting");
		{
			// which CPU cores this app may use; ask for all 16 so decoding can spread out
			cpuset_t m;
			CPU_ZERO(&m);
			int r = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(m), &m);
			AppLog("cpus online %ld, affinity %d: %d cores", sysconf(_SC_NPROCESSORS_ONLN), r, CPU_COUNT(&m));
			cpuset_t all;
			CPU_ZERO(&all);
			for(int c = 0; c < 16; c++) CPU_SET(c, &all);
			r = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(all), &all);
			CPU_ZERO(&m);
			cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(m), &m);
			AppLog("set affinity to all: %d, now %d cores", r, CPU_COUNT(&m));
		}
		set = LoadSettings();
		cfg = LoadConfig();
		FontInit();
		auto loadImg = [](const char *path, UiImg &img) {
			int n = 0;
			uint8_t *d = stbi_load(path, &img.w, &img.h, &n, 4);
			if(d) { img.px.assign(d, d + img.w * img.h * 4); stbi_image_free(d); }
			AppLog("image %s: %dx%d %s", path, img.w, img.h, d ? "ok" : "missing");
		};
		loadImg("/app0/assets/ui/ps5.png", g_imgConsole);
		loadImg("/app0/assets/ui/logo.png", g_imgLogo);

		sceUserServiceInitialize(nullptr);
		sceUserServiceGetInitialUser(&userId);
		int r = scePadInit();
		pad = scePadOpen(userId, 0, 0, nullptr);
		AppLog("user %d, scePadInit %d, pad handle %d", userId, r, pad);
		if(pad >= 0)
		{
			scePadSetMotionSensorState(pad, true);
			uint8_t lb[4] = {20, 60, 255, 0};
			scePadSetLightBar(pad, lb);
		}
		InitAudio();

		gfx.want120 = set.hz120 != 0;
		if(!gfx.Init()) { AppLog("graphics init failed"); return false; }

		chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, AppChiakiLog, this);
		ChiakiErrorCode err = chiaki_lib_init();
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("chiaki init failed: ") + chiaki_error_string(err)); return true; }
		chiaki_orientation_tracker_init(&orient);
		chiaki_accel_new_zero_set_inactive(&accelZero, true);

		AppLog("PS5 host: %s, %d profile(s)", cfg.host.empty() ? "(not set)" : cfg.host.c_str(), (int)cfg.profiles.size());
		GoProfiles();
		return true;
	}

	void Fail(const std::string &m) { AppLog("ERROR: %s", m.c_str()); Status(m); state = State::Error; }

	// ---- audio
	void InitAudio()
	{
		audioRing.assign(48000 * 2, 0); // 0.5 s stereo
		int ir = sceAudioOutInit();
		struct { int user, type; } tries[] = {{userId, 0}, {255, 0}, {0, 0}, {userId, 1}, {255, 1}};
		for(auto &t : tries)
		{
			audioPort = sceAudioOutOpen(t.user, t.type, 0, 256, 48000, 1);
			AppLog("audio init %d, open(user %d, type %d) -> 0x%x", ir, t.user, t.type, (unsigned)audioPort);
			if(audioPort >= 0) break;
		}
		if(audioPort < 0) return;
		std::thread([this]() {
			int16_t buf[512];
			while(running)
			{
				{
					std::lock_guard<std::mutex> l(audioMutex);
					if(audioFill >= 512)
					{
						for(int i = 0; i < 512; i++)
						{
							buf[i] = audioRing[audioRead];
							audioRead = (audioRead + 1) % audioRing.size();
						}
						audioFill -= 512;
					}
					else memset(buf, 0, sizeof(buf));
				}
				sceAudioOutOutput(audioPort, buf);
			}
		}).detach();
	}

	uint32_t audioChannels = 2;
	void OnAudioSettings(uint32_t ch, uint32_t rate)
	{
		audioChannels = ch;
		AppLog("audio: %u ch, %u Hz", ch, rate);
	}

	void OnAudioFrame(int16_t *b, size_t frames)
	{
		std::lock_guard<std::mutex> l(audioMutex);
		for(size_t i = 0; i < frames; i++)
		{
			int16_t L = b[i * audioChannels], R = audioChannels > 1 ? b[i * audioChannels + 1] : L;
			audioRing[audioWrite] = L;
			audioRing[(audioWrite + 1) % audioRing.size()] = R;
			audioWrite = (audioWrite + 2) % audioRing.size();
			audioFill += 2;
		}
		// keep latency low: drop to ~40 ms when more than ~120 ms is queued
		if(audioFill > 48000 * 2 * 120 / 1000)
		{
			size_t drop = audioFill - 48000 * 2 * 40 / 1000;
			drop &= ~(size_t)1;
			audioRead = (audioRead + drop) % audioRing.size();
			audioFill -= drop;
		}
		if(audioFill > audioRing.size()) { audioFill = 0; audioRead = audioWrite; }
	}

	// ---- video decode (chiaki's thread)
	bool InitDecoder(bool hevc)
	{
		FreeDecoder();
		const AVCodec *codec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
		if(!codec) { AppLog("decoder not found"); return false; }
		dec = avcodec_alloc_context3(codec);
		// 0: slice/WPP threads only (no added delay); 1-2: frame threads too (each extra thread adds a frame of delay
		// but decodes frames side by side, for streams one core can't keep up with)
		if(set.decmode == 0) { dec->thread_count = 8; dec->thread_type = FF_THREAD_SLICE; dec->flags |= AV_CODEC_FLAG_LOW_DELAY; }
		else { dec->thread_count = set.decmode == 1 ? 2 : 4; dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE; }
		AppLog("decoder mode %d: %d threads, type %d", set.decmode, dec->thread_count, dec->thread_type);
		dec->flags2 |= AV_CODEC_FLAG2_FAST;
		if(avcodec_open2(dec, codec, nullptr) < 0) { AppLog("avcodec_open2 failed"); return false; }
		avf = av_frame_alloc();
		pkt = av_packet_alloc();
		AppLog("decoder: %s", codec->name);
		return true;
	}

	void FreeDecoder()
	{
		if(dec) avcodec_free_context(&dec);
		if(avf) av_frame_free(&avf);
		if(pkt) av_packet_free(&pkt);
	}

	void Decode(uint8_t *buf, size_t size)
	{
		if(!dec) return;
		pkt->data = buf;
		pkt->size = (int)size;
		if(avcodec_send_packet(dec, pkt) < 0) return;
		while(avcodec_receive_frame(dec, avf) == 0)
		{
			if(avf->format != AV_PIX_FMT_YUV420P && avf->format != AV_PIX_FMT_YUVJ420P)
			{
				static bool warned = false;
				if(!warned) { AppLog("unexpected pixel format %d", avf->format); warned = true; }
				continue;
			}
			uint32_t w = avf->width & ~1u, h = avf->height & ~1u;
			if(w > VIDEO_MAX_W || h > VIDEO_MAX_H) continue;
			texWLog = w; texHLog = h;
			std::lock_guard<std::mutex> l(frameMutex);
			frameBuf.resize((size_t)w * h * 3 / 2);
			uint8_t *d = frameBuf.data();
			for(uint32_t y = 0; y < h; y++) memcpy(d + (size_t)y * w, avf->data[0] + (size_t)y * avf->linesize[0], w);
			d += (size_t)w * h;
			for(int p = 1; p <= 2; p++)
			{
				for(uint32_t y = 0; y < h / 2; y++) memcpy(d + (size_t)y * (w / 2), avf->data[p] + (size_t)y * avf->linesize[p], w / 2);
				d += (size_t)(w / 2) * (h / 2);
			}
			frameW = w;
			frameH = h;
			frameSeq++;
			frameCv.notify_one();
		}
	}

	// ---- profile / registration / session
	void GoProfiles()
	{
		state = State::ProfileSelect;
		if(profileIdx >= ProfileItems()) profileIdx = 0;
		if(cfg.host.empty()) Status("Welcome! Set your console's IP, then add a profile");
		else if(cfg.profiles.empty()) Status("Add a profile to get started");
		else Status("Who's playing?");
		uiDirty = true;
	}

	// The main page: the profiles, then "+ Add profile" (up to 6 profiles), "Console IP" and "Controls & help".
	// toolbar buttons follow the profiles in the focus order
	int ItemAdd() { return (int)cfg.profiles.size(); }
	int ItemIp() { return ItemAdd() + 1; }
	int ItemHelp() { return ItemAdd() + 2; }
	int ProfileItems() { return ItemAdd() + 3; }

	void OpenKeyboard(std::string *target, const std::string &title, int mode, size_t maxLen, State back)
	{
		kbTarget = target;
		kbBuf = *target;
		kbTitle = title;
		kbMode = mode;
		kbMax = maxLen;
		kbBack = back;
		kbShift = false;
		kbRow = mode == 1 ? 0 : 1;
		kbCol = 0;
		state = State::Keyboard;
		uiDirty = true;
	}

	static const std::vector<std::string> &KbRows(int mode)
	{
		static const std::vector<std::string> text = {"1234567890", "qwertyuiop", "asdfghjkl=", "zxcvbnm+/-"};
		static const std::vector<std::string> ip = {"123", "456", "789", ".0"};
		return mode == 1 ? ip : text;
	}
	static const std::vector<std::string> &KbActions(int mode)
	{
		static const std::vector<std::string> text = {"Shift", "Space", "Delete", "Done"};
		static const std::vector<std::string> ip = {"Delete", "Done"};
		return mode == 1 ? ip : text;
	}
	int KbRowLen(int r) { const auto &rows = KbRows(kbMode); return r < (int)rows.size() ? (int)rows[r].size() : (int)KbActions(kbMode).size(); }

	void KbDone()
	{
		std::string v;
		for(char c : kbBuf) v += c; // keep as typed
		while(!v.empty() && v.back() == ' ') v.pop_back();
		while(!v.empty() && v.front() == ' ') v.erase(v.begin());
		*kbTarget = v;
		if(kbTarget == &cfg.host) { SaveConfig(cfg); AppLog("console IP set to %s", cfg.host.c_str()); }
		if(kbBack == State::ProfileSelect) GoProfiles();
		else { state = kbBack; uiDirty = true; }
	}

	void KbType(char c)
	{
		if(kbBuf.size() >= kbMax) return;
		if(kbShift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		kbBuf += c;
		uiDirty = true;
	}

	void KbPress()
	{
		const auto &rows = KbRows(kbMode);
		if(kbRow < (int)rows.size()) { KbType(rows[kbRow][kbCol]); return; }
		const std::string &a = KbActions(kbMode)[kbCol];
		if(a == "Shift") kbShift = !kbShift;
		else if(a == "Space") KbType(' ');
		else if(a == "Delete") { if(!kbBuf.empty()) kbBuf.pop_back(); }
		else if(a == "Done") { KbDone(); return; }
		uiDirty = true;
	}

	void OpenEditor(int idx)
	{
		editIdx = idx;
		editSel = 0;
		editMsg.clear();
		if(idx >= 0) { editName = cfg.profiles[idx].name; editAcc = cfg.profiles[idx].account_b64; }
		else { editName.clear(); editAcc.clear(); }
		state = State::EditProfile;
		uiDirty = true;
	}
	int EditItems() { return editIdx >= 0 ? 5 : 4; } // name, account, fill from this PS5, save, (delete)

	// The PSN account signed in on this PS5 (and its user name), looked up at run time like the splash call.
	bool ReadLocalAccount(std::string &acc, std::string &name)
	{
		int mod = sceKernelLoadStartModule("/system/common/lib/libSceNpManager.sprx", 0, nullptr, 0, nullptr, nullptr);
		void *fn = nullptr;
		if(mod >= 0) sceKernelDlsym(mod, "sceNpGetAccountIdA", &fn);
		uint64_t id = 0;
		int r = fn ? ((int (*)(int, uint64_t *))fn)(userId, &id) : -1;
		AppLog("sceNpGetAccountIdA: module %d, result %d", mod, r);
		if(r < 0 || id == 0) return false;
		uint8_t b[8];
		for(int i = 0; i < 8; i++) b[i] = (uint8_t)(id >> (8 * i));
		char enc[32] = {0};
		chiaki_base64_encode(b, sizeof(b), enc, sizeof(enc));
		acc = enc;
		int um = sceKernelLoadStartModule("/system/common/lib/libSceUserService.sprx", 0, nullptr, 0, nullptr, nullptr);
		void *gn = nullptr;
		if(um >= 0) sceKernelDlsym(um, "sceUserServiceGetUserName", &gn);
		char nm[64] = {0};
		if(gn && ((int (*)(int, char *, size_t))gn)(userId, nm, sizeof(nm) - 1) >= 0) name = nm;
		return true;
	}

	void EditActivate()
	{
		editMsg.clear();
		if(editSel == 0) OpenKeyboard(&editName, "Profile name", 0, 16, State::EditProfile);
		else if(editSel == 1) OpenKeyboard(&editAcc, "PSN account ID (base64 or number)", 0, 24, State::EditProfile);
		else if(editSel == 2)
		{
			std::string acc, nm;
			if(ReadLocalAccount(acc, nm))
			{
				editAcc = acc;
				if(editName.empty() && !nm.empty()) editName = nm.substr(0, 16);
				editMsg = "Filled in from this PS5's signed-in account";
			}
			else editMsg = "Couldn't read this PS5's account (is a PSN account signed in?)";
		}
		else if(editSel == 3)
		{
			std::string acc;
			if(editName.empty()) { editMsg = "Enter a name"; editSel = 0; }
			else if(!NormalizeAccount(editAcc, acc)) { editMsg = "That account ID isn't valid"; editSel = 1; }
			else
			{
				for(size_t i = 0; i < cfg.profiles.size(); i++)
					if((int)i != editIdx && cfg.profiles[i].name == editName) { editMsg = "Another profile already has that name"; uiDirty = true; return; }
				if(editIdx >= 0)
				{
					Profile &p = cfg.profiles[editIdx];
					if(p.account_b64 != acc) remove(HostPath(p.name).c_str()); // another account: link again
					else if(p.name != editName) rename(HostPath(p.name).c_str(), HostPath(editName).c_str());
					p.name = editName;
					p.account_b64 = acc;
				}
				else
				{
					cfg.profiles.push_back({editName, acc});
					profileIdx = (int)cfg.profiles.size() - 1;
				}
				SaveConfig(cfg);
				AppLog("profile saved: %s", editName.c_str());
				GoProfiles();
				return;
			}
		}
		else if(editSel == 4 && editIdx >= 0)
		{
			remove(HostPath(cfg.profiles[editIdx].name).c_str());
			cfg.profiles.erase(cfg.profiles.begin() + editIdx);
			SaveConfig(cfg);
			profileIdx = 0;
			GoProfiles();
			return;
		}
		uiDirty = true;
	}

	void ChooseProfile(int i)
	{
		if(cfg.host.empty())
		{
			OpenKeyboard(&cfg.host, "Console IP (the PS5 you stream from)", 1, 15, State::ProfileSelect);
			return;
		}
		const Profile &p = cfg.profiles[i];
		profileName = p.name;
		size_t outSize = sizeof(accountId);
		if(chiaki_base64_decode(p.account_b64.c_str(), p.account_b64.size(), accountId, &outSize) != CHIAKI_ERR_SUCCESS || outSize != sizeof(accountId))
		{
			Fail("Account ID for " + p.name + " is not valid (needs 8 bytes base64)");
			return;
		}
		AppLog("profile %s", p.name.c_str());
		if(LoadHost(profileName, &host)) Connect();
		else
		{
			memset(pinDigits, 0, sizeof(pinDigits));
			pinCursor = 0;
			state = State::PinEntry;
			Status(p.name + ": on the other PS5 go to Settings > System > Remote Play > Link Device, then enter the PIN");
		}
	}

	void StartRegist()
	{
		uint32_t pin = 0;
		for(int i = 0; i < 8; i++) pin = pin * 10 + pinDigits[i];
		ChiakiRegistInfo info{};
		info.target = CHIAKI_TARGET_PS5_1;
		info.host = cfg.host.c_str();
		info.broadcast = false;
		info.psn_online_id = nullptr;
		memcpy(info.psn_account_id, accountId, sizeof(accountId));
		info.pin = pin;
		info.console_pin = 0;
		info.holepunch_info = nullptr;
		info.rudp = nullptr;
		state = State::Registering;
		Status("Registering with the PS5...");
		ChiakiErrorCode err = chiaki_regist_start(&regist, &log, &info, [](ChiakiRegistEvent *ev, void *user) {
			App *self = (App *)user;
			if(ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
			{
				self->host = *ev->registered_host;
				SaveHost(self->profileName, &self->host);
				AppLog("registered with %s", self->host.server_nickname);
				self->wantConnect = true;
			}
			else
			{
				self->Status("Registration failed - check the PIN, then try again");
				self->state = State::PinEntry;
			}
			self->registDone = true;
		}, this);
		if(err != CHIAKI_ERR_SUCCESS) Fail(std::string("Registration could not start: ") + chiaki_error_string(err));
		else registActive = true;
	}

	void Connect()
	{
		state = State::Connecting;
		Status("Connecting to " + cfg.host + "...");
		uint64_t cred = strtoull(std::string(host.rp_regist_key, strnlen(host.rp_regist_key, sizeof(host.rp_regist_key))).c_str(), nullptr, 16);
		chiaki_discovery_wakeup(&log, nullptr, cfg.host.c_str(), cred, true);

		bool hevc = !set.h264 || set.res >= 2; // above 1080p needs HEVC
		if(!InitDecoder(hevc)) { Fail("Could not start the video decoder"); return; }
		StartDecodeThread();

		// Frame gen 30->60 asks the PS5 for 30 fps; 60->120 needs a 120 Hz display.
		int fgMode = set.framegen;
		if(fgMode == 2 && gfx.displayHz < 100.0) { AppLog("60->120 needs 120 Hz; display is %.1f Hz, using off", gfx.displayHz); fgMode = 0; }
		if(!gfx.fgReady) fgMode = 0;
		activeFg = fgMode;
		streamIs30 = fgMode == 1;
		fgReset = true;

		ChiakiConnectInfo ci{};
		ci.ps5 = true;
		ci.host = cfg.host.c_str();
		memcpy(ci.regist_key, host.rp_regist_key, sizeof(ci.regist_key));
		memcpy(ci.morning, host.rp_key, sizeof(ci.morning));
		chiaki_connect_video_profile_preset(&ci.video_profile,
			set.res == 0 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
			streamIs30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
		// experimental: ask for more than 1080p; the console answers with what it can do (auto downgrade)
		if(set.res == 2) { ci.video_profile.width = 2560; ci.video_profile.height = 1440; }
		if(set.res == 3) { ci.video_profile.width = 3840; ci.video_profile.height = 2160; }
		ci.video_profile.codec = hevc ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
		ci.video_profile.bitrate = kBitrates[set.bitrate];
		ci.video_profile_auto_downgrade = true;
		ci.enable_keyboard = false;
		ci.enable_dualsense = true;
		ci.auto_regist = false;
		ci.holepunch_session = nullptr;
		ci.rudp_sock = nullptr;
		memcpy(ci.psn_account_id, accountId, sizeof(accountId));
		ci.packet_loss_max = 0.05;
		ci.enable_idr_on_fec_failure = true;
		AppLog("connect: %s %s %d kbps, frame gen %d", ResName(set.res), hevc ? "HEVC" : "H.264", kBitrates[set.bitrate], fgMode);

		ChiakiErrorCode err = chiaki_session_init(&session, &ci, &log);
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("Session init failed: ") + chiaki_error_string(err)); return; }
		chiaki_opus_decoder_init(&opus, &log);
		chiaki_opus_decoder_set_cb(&opus,
			[](uint32_t ch, uint32_t rate, void *u) { ((App *)u)->OnAudioSettings(ch, rate); },
			[](int16_t *b, size_t n, void *u) { ((App *)u)->OnAudioFrame(b, n); }, this);
		ChiakiAudioSink sink;
		chiaki_opus_decoder_get_sink(&opus, &sink);
		chiaki_session_set_audio_sink(&session, &sink);
		chiaki_session_set_video_sample_cb(&session, [](uint8_t *buf, size_t size, int32_t, bool, void *u) -> bool {
			((App *)u)->QueuePacket(buf, size);
			return true;
		}, this);
		chiaki_session_set_event_cb(&session, [](ChiakiEvent *ev, void *u) {
			App *self = (App *)u;
			switch(ev->type)
			{
				case CHIAKI_EVENT_CONNECTED:
					AppLog("connected");
					self->inUseRetries = 0;
					self->state = State::Streaming;
					self->uiDirty = true;
					break;
				case CHIAKI_EVENT_RUMBLE:
					self->rumbleL = ev->rumble.left;
					self->rumbleR = ev->rumble.right;
					self->rumbleDirty = true;
					break;
				case CHIAKI_EVENT_QUIT:
				{
					std::string m = std::string("Disconnected: ") + chiaki_quit_reason_string(ev->quit.reason);
					if(ev->quit.reason_str) m += std::string(" - ") + ev->quit.reason_str;
					AppLog("%s", m.c_str());
					self->sessionQuit = true;
					if(ev->quit.reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE && self->inUseRetries < 10)
					{
						// the console lets go of the last session a few seconds after it ends: try again shortly
						self->inUseRetries++;
						self->retryAt = NowUs() + 2000000;
						self->Status("The console is still finishing the last session... retrying (" + std::to_string(self->inUseRetries) + "/10)");
						self->state = State::Connecting;
						break;
					}
					self->Status(m + "  (X: reconnect, O: profiles)");
					self->state = State::Error;
					break;
				}
				default:
					break;
			}
		}, this);
		err = chiaki_session_start(&session);
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("Session start failed: ") + chiaki_error_string(err)); return; }
		sessionActive = true;
	}
	int activeFg = 0;

	void StopSession()
	{
		if(!sessionActive) return;
		chiaki_session_stop(&session);
		chiaki_session_join(&session);
		chiaki_session_fini(&session);
		chiaki_opus_decoder_fini(&opus);
		sessionActive = false;
		StopDecodeThread();
		{
			std::lock_guard<std::mutex> l(frameMutex);
			FreeDecoder();
		}
		haveVideo = false;
		menuOpen = false;
		uiDirty = true;
	}

	// ---- input
	void SendController(const PadData &p)
	{
		ChiakiControllerState cs;
		chiaki_controller_state_set_idle(&cs);
		uint32_t b = p.buttons;
		struct { uint32_t pad, ch; } map[] = {
			{PAD_CROSS, CHIAKI_CONTROLLER_BUTTON_CROSS}, {PAD_CIRCLE, CHIAKI_CONTROLLER_BUTTON_MOON},
			{PAD_SQUARE, CHIAKI_CONTROLLER_BUTTON_BOX}, {PAD_TRIANGLE, CHIAKI_CONTROLLER_BUTTON_PYRAMID},
			{PAD_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT}, {PAD_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
			{PAD_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP}, {PAD_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
			{PAD_L1, CHIAKI_CONTROLLER_BUTTON_L1}, {PAD_R1, CHIAKI_CONTROLLER_BUTTON_R1},
			{PAD_L3, CHIAKI_CONTROLLER_BUTTON_L3}, {PAD_R3, CHIAKI_CONTROLLER_BUTTON_R3},
			{PAD_OPTIONS, CHIAKI_CONTROLLER_BUTTON_OPTIONS}, {PAD_CREATE, CHIAKI_CONTROLLER_BUTTON_SHARE},
			{PAD_TOUCHPAD, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},
		};
		if(holdUntilRelease)
		{
			if(b == 0) holdUntilRelease = false;
			else b = 0;
		}
		// Options is only forwarded as a tap on release; while it is held, Up/Down are combo keys.
		if(b & PAD_OPTIONS) b &= ~(PAD_UP | PAD_DOWN);
		b &= ~PAD_OPTIONS;
		if(NowUs() < optionsTapUntil) b |= PAD_OPTIONS;
		for(auto &m : map) if(b & m.pad) cs.buttons |= m.ch;
		if(NowUs() < psUntil)
		{
			cs.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
		}
		cs.l2_state = p.l2;
		cs.r2_state = p.r2;
		auto axis = [](uint8_t v) -> int16_t { int x = ((int)v - 128) * 256; return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); };
		cs.left_x = axis(p.lx);
		cs.left_y = axis(p.ly);
		cs.right_x = axis(p.rx);
		cs.right_y = axis(p.ry);
		for(int i = 0; i < 2; i++)
		{
			if(i < p.touchNum)
			{
				cs.touches[i].id = (int8_t)(p.touch[i].id & 0x7f);
				cs.touches[i].x = p.touch[i].x > 1919 ? 1919 : p.touch[i].x;
				cs.touches[i].y = p.touch[i].y > 1079 ? 1079 : p.touch[i].y;
			}
			else cs.touches[i].id = -1;
		}
		chiaki_orientation_tracker_update(&orient, p.gx, p.gy, p.gz, p.ax, p.ay, p.az, &accelZero, false, (uint32_t)NowUs());
		chiaki_orientation_tracker_apply_to_controller_state(&orient, &cs);
		chiaki_session_set_controller_state(&session, &cs);
	}

	static const int MENU_ITEMS = 14;
	static const char *ResName(int r) { static const char *n[] = {"720p", "1080p", "1440p", "4K"}; return n[r & 3]; }
	std::string MenuLabel(int i)
	{
		char b[128];
		static const char *lvl[] = {"Off", "Low", "High"};
		switch(i)
		{
			case 0: snprintf(b, sizeof(b), "Detail restore      < %s >", lvl[set.restore]); break;
			case 1: snprintf(b, sizeof(b), "Deband              < %s >", set.deband ? "On" : "Off"); break;
			case 2: snprintf(b, sizeof(b), "Sharpening          < %d >", set.sharpen); break;
			case 3: snprintf(b, sizeof(b), "Color boost         < %s >", lvl[set.boost]); break;
			case 4: snprintf(b, sizeof(b), "Frame gen *         < %s >", set.framegen == 0 ? "Off" : set.framegen == 1 ? "30 -> 60" : "60 -> 120"); break;
			case 5: snprintf(b, sizeof(b), "FPS counter         < %s >", set.fps ? "On" : "Off"); break;
			case 6: snprintf(b, sizeof(b), "Resolution *        < %s%s >", ResName(set.res), set.res >= 2 ? " (beta)" : ""); break;
			case 7: snprintf(b, sizeof(b), "Bitrate *           < %d Mbps >", kBitrates[set.bitrate] / 1000); break;
			case 8: snprintf(b, sizeof(b), "Codec *             < %s >", set.h264 ? "H.264" : "HEVC"); break;
			case 9: snprintf(b, sizeof(b), "Decoder *           < %s >", set.decmode == 0 ? "Low latency" : set.decmode == 1 ? "Fast" : "Fastest"); break;
			case 10: snprintf(b, sizeof(b), "Press PS button  (or Options+Down)"); break;
			case 11: snprintf(b, sizeof(b), "Reconnect now (applies *)"); break;
			case 12: snprintf(b, sizeof(b), "Disconnect"); break;
			default: snprintf(b, sizeof(b), "Close menu"); break;
		}
		return b;
	}

	void MenuAdjust(int dir)
	{
		auto cyc = [&](int &v, int n) { v = (v + dir + n) % n; };
		switch(menuIdx)
		{
			case 0: cyc(set.restore, 3); break;
			case 1: cyc(set.deband, 2); break;
			case 2: set.sharpen = set.sharpen + dir < 0 ? 0 : set.sharpen + dir > 10 ? 10 : set.sharpen + dir; break;
			case 3: cyc(set.boost, 3); break;
			case 4: cyc(set.framegen, 3); break;
			case 5: cyc(set.fps, 2); break;
			case 6: cyc(set.res, 4); break;
			case 7: set.bitrate = set.bitrate + dir < 0 ? 0 : set.bitrate + dir >= kBitrateCount ? kBitrateCount - 1 : set.bitrate + dir; break;
			case 8: cyc(set.h264, 2); break;
			case 9: cyc(set.decmode, 3); break;
			default: return;
		}
		SaveSettings(set);
		uiDirty = true;
	}

	uint64_t decSum = 0, decMax = 0; int decN = 0; uint32_t texWLog = 0, texHLog = 0;
	bool reconnectRequested = false, disconnectRequested = false;
	int inUseRetries = 0;
	uint64_t retryAt = 0;
	void MenuActivate()
	{
		holdUntilRelease = true;
		if(menuIdx == 10) { menuOpen = false; psUntil = NowUs() + 250000; AppLog("PS button sent (menu)"); }
		else if(menuIdx == 11) { reconnectRequested = true; menuOpen = false; }
		else if(menuIdx == 12) { disconnectRequested = true; menuOpen = false; }
		else if(menuIdx == 13) menuOpen = false;
		else MenuAdjust(1);
		uiDirty = true;
	}

	void HandleInput()
	{
		PadData p;
		memset(&p, 0, sizeof(p));
		if(pad < 0 || scePadReadState(pad, &p) < 0) return;
		uint32_t b = p.buttons;
		uint32_t pressed = b & ~prevButtons;
		prevButtons = b;

		if(rumbleDirty.exchange(false))
		{
			uint8_t v[2] = {rumbleL.load(), rumbleR.load()};
			scePadSetVibration(pad, v);
		}

		// Combos: hold OPTIONS, then press D-pad UP (menu) or D-pad DOWN (PS button).
		// A plain OPTIONS press is sent to the game as a tap when released.
		uint64_t now = NowUs();
		bool optHeld = (b & PAD_OPTIONS) != 0;
		if(optHeld && !optionsHeld) optionsUsed = false;
		if(optHeld && (pressed & PAD_UP) && state == State::Streaming)
		{
			menuOpen = !menuOpen;
			optionsUsed = true;
			holdUntilRelease = true;
			uiDirty = true;
			AppLog(menuOpen ? "Menu opened (Options+Up)" : "Menu closed (Options+Up)");
		}
		if(optHeld && (pressed & PAD_DOWN) && state == State::Streaming && !menuOpen)
		{
			psUntil = now + 200000;
			optionsUsed = true;
			AppLog("PS button sent (Options+Down)");
		}
		if(!optHeld && optionsHeld && !optionsUsed && state == State::Streaming && !menuOpen)
			optionsTapUntil = now + 120000;
		optionsHeld = optHeld;
		bool comboFired = optionsUsed && optHeld;

		State st = state;
		if(st == State::Streaming)
		{
			if(menuOpen)
			{
				if(sessionActive)
				{
					ChiakiControllerState idle;
					chiaki_controller_state_set_idle(&idle);
					chiaki_session_set_controller_state(&session, &idle);
				}
				if(comboFired) return; // don't treat the combo buttons as menu input
				if(pressed & PAD_UP) { menuIdx = (menuIdx + MENU_ITEMS - 1) % MENU_ITEMS; uiDirty = true; }
				if(pressed & PAD_DOWN) { menuIdx = (menuIdx + 1) % MENU_ITEMS; uiDirty = true; }
				if(pressed & PAD_LEFT) MenuAdjust(-1);
				if(pressed & PAD_RIGHT) MenuAdjust(1);
				if(pressed & PAD_CROSS) MenuActivate();
				if(pressed & PAD_CIRCLE) { menuOpen = false; holdUntilRelease = true; uiDirty = true; }
			}
			else if(sessionActive) SendController(p);
			return;
		}
		if(st == State::ProfileSelect)
		{
			int np = (int)cfg.profiles.size();
			bool inBar = profileIdx >= np;
			if(pressed & PAD_UP)
			{
				if(!inBar) profileIdx = profileIdx == 0 ? ItemAdd() : profileIdx - 1;
				else if(np) profileIdx = np - 1;
				uiDirty = true;
			}
			if(pressed & PAD_DOWN)
			{
				if(inBar) { if(np) profileIdx = 0; }
				else profileIdx = profileIdx + 1 < np ? profileIdx + 1 : ItemAdd();
				uiDirty = true;
			}
			if(inBar && (pressed & PAD_LEFT)) { profileIdx = profileIdx > ItemAdd() ? profileIdx - 1 : ItemHelp(); uiDirty = true; }
			if(inBar && (pressed & PAD_RIGHT)) { profileIdx = profileIdx < ItemHelp() ? profileIdx + 1 : ItemAdd(); uiDirty = true; }
			bool isProfile = profileIdx < np;
			if(pressed & PAD_CROSS)
			{
				if(isProfile) ChooseProfile(profileIdx);
				else if(profileIdx == ItemAdd())
				{
					if(np >= 8) Status("You can have up to 8 profiles");
					else OpenEditor(-1);
				}
				else if(profileIdx == ItemIp()) OpenKeyboard(&cfg.host, "Console IP (the PS5 you stream from)", 1, 15, State::ProfileSelect);
				else if(profileIdx == ItemHelp()) { state = State::Help; uiDirty = true; }
			}
			if((pressed & PAD_TRIANGLE) && isProfile) OpenEditor(profileIdx);
		}
		else if(st == State::EditProfile)
		{
			int n = EditItems();
			if(pressed & PAD_UP) { editSel = (editSel + n - 1) % n; uiDirty = true; }
			if(pressed & PAD_DOWN) { editSel = (editSel + 1) % n; uiDirty = true; }
			if(pressed & PAD_CROSS) EditActivate();
			if(pressed & PAD_CIRCLE) GoProfiles();
		}
		else if(st == State::Keyboard)
		{
			int rows = (int)KbRows(kbMode).size() + 1;
			if(pressed & PAD_UP) { kbRow = (kbRow + rows - 1) % rows; uiDirty = true; }
			if(pressed & PAD_DOWN) { kbRow = (kbRow + 1) % rows; uiDirty = true; }
			if(pressed & PAD_LEFT) { kbCol--; uiDirty = true; }
			if(pressed & PAD_RIGHT) { kbCol++; uiDirty = true; }
			int len = KbRowLen(kbRow);
			if(kbCol < 0) kbCol = len - 1;
			if(kbCol >= len) kbCol = (pressed & PAD_RIGHT) ? 0 : len - 1;
			if(pressed & PAD_CROSS) KbPress();
			else if(pressed & PAD_SQUARE) { if(!kbBuf.empty()) kbBuf.pop_back(); uiDirty = true; }
			else if((pressed & PAD_TRIANGLE) && kbMode == 0) KbType(' ');
			else if((pressed & (PAD_L1 | PAD_R1)) && kbMode == 0) { kbShift = !kbShift; uiDirty = true; }
			else if(pressed & PAD_OPTIONS) KbDone();
			else if(pressed & PAD_CIRCLE)
			{
				if(kbBack == State::ProfileSelect) GoProfiles();
				else { state = kbBack; uiDirty = true; }
			}
		}
		else if(st == State::Help)
		{
			if(pressed & (PAD_CIRCLE | PAD_CROSS)) GoProfiles();
		}
		else if(st == State::PinEntry)
		{
			if(pressed & PAD_LEFT) { pinCursor = (pinCursor + 7) % 8; uiDirty = true; }
			if(pressed & PAD_RIGHT) { pinCursor = (pinCursor + 1) % 8; uiDirty = true; }
			if(pressed & PAD_UP) { pinDigits[pinCursor] = (pinDigits[pinCursor] + 1) % 10; uiDirty = true; }
			if(pressed & PAD_DOWN) { pinDigits[pinCursor] = (pinDigits[pinCursor] + 9) % 10; uiDirty = true; }
			if(pressed & PAD_CROSS) StartRegist();
			if(pressed & PAD_CIRCLE) GoProfiles();
		}
		else if(st == State::Error)
		{
			if(pressed & PAD_CROSS)
			{
				StopSession();
				if(!profileName.empty() && LoadHost(profileName, &host)) Connect();
				else GoProfiles();
			}
			if(pressed & PAD_CIRCLE) { StopSession(); GoProfiles(); }
		}
	}

	// ---- UI drawing (chiaki-ng style)
	static uint32_t CBg() { return RGBA(48, 48, 48, 255); }
	static uint32_t CBar() { return RGBA(33, 33, 33, 255); }
	static uint32_t CSurface() { return RGBA(66, 66, 66, 255); }
	static uint32_t CAccent() { return RGBA(0, 167, 255, 255); }
	static uint32_t CText() { return RGBA(255, 255, 255, 255); }
	static uint32_t CDim() { return RGBA(179, 179, 179, 255); }
	static uint32_t CBtn() { return RGBA(88, 88, 88, 255); }

	static void Button(int x, int y, int w, int h, const char *label, bool focus, uint32_t text = 0)
	{
		UiRoundRect(x, y, w, h, 6, focus ? CAccent() : CBtn());
		int tw = UiTextWidth(label, 1);
		UiText(x + (w - tw) / 2, y + (h - 14) / 2, label, text ? text : CText(), 1);
	}

	static void Field(int x, int y, int w, const char *label, const std::string &value, const char *empty, bool focus)
	{
		UiText(x, y, label, focus ? CAccent() : CDim(), 1);
		UiRoundRect(x, y + 18, w, 38, 4, RGBA(255, 255, 255, focus ? 34 : 20));
		UiRect(x, y + 54, w, focus ? 2 : 1, focus ? CAccent() : RGBA(255, 255, 255, 90));
		UiText(x + 12, y + 30, value.empty() ? empty : value.c_str(), value.empty() ? CDim() : CText(), 1);
	}

	void Dialog(int x, int y, int w, int h, const char *title)
	{
		UiRoundRect(x + 2, y + 4, w, h, 10, RGBA(0, 0, 0, 90));
		UiRoundRect(x, y, w, h, 10, CSurface());
		UiText(x + 24, y + 18, title, CText(), 2);
	}

	void DrawMainPage(const std::string &status, bool active)
	{
		UiRect(0, 0, 960, 540, CBg());
		UiImage(g_imgLogo, 480 - g_imgLogo.w / (2 * U), 290 - g_imgLogo.h / (2 * U), 0.13f);

		// toolbar
		UiRect(0, 0, 960, 56, CBar());
		UiRect(0, 56, 960, 2, RGBA(0, 0, 0, 70));
		UiText(20, 14, "chiaki4ps5", CText(), 2);
		int np = (int)cfg.profiles.size();
		Button(545, 10, 160, 36, "+ Add Profile", active && profileIdx == ItemAdd());
		std::string ip = cfg.host.empty() ? "Console IP" : cfg.host;
		Button(715, 10, 140, 36, ip.c_str(), active && profileIdx == ItemIp());
		Button(865, 10, 80, 36, "Help", active && profileIdx == ItemHelp());

		// the profiles, like chiaki-ng's console list
		if(np == 0)
		{
			UiTextCentered(240, "No profiles yet", CText(), 2);
			UiTextCentered(280, cfg.host.empty() ? "Set your Console IP, then choose + Add Profile at the top"
				: "Choose + Add Profile at the top to get started", CDim(), 1);
		}
		int step = np <= 4 ? 100 : (np <= 5 ? 88 : 440 / np);
		for(int i = 0; i < np; i++)
		{
			int y = 62 + i * step;
			bool sel = active && i == profileIdx;
			if(sel)
			{
				UiRect(0, y, 960, step, RGBA(255, 255, 255, 30));
				UiRect(0, y, 5, step, CAccent());
			}
			if(i > 0) UiRect(20, y, 920, 1, RGBA(255, 255, 255, 18));
			UiImage(g_imgConsole, 30, y + (step - g_imgConsole.h / U) / 2, 1.0f);
			const Profile &p = cfg.profiles[i];
			bool linked = false;
			{ ChiakiRegisteredHost tmp; linked = LoadHost(p.name, &tmp); }
			int ty = y + step / 2 - 30;
			UiText(220, ty, p.name.c_str(), CText(), 2);
			std::string addr = "Address: " + (cfg.host.empty() ? std::string("not set") : cfg.host);
			UiText(220, ty + 34, addr.c_str(), CDim(), 1);
			UiText(220, ty + 52, linked ? "ID: registered" : "ID: unregistered", CDim(), 1);
			UiText(620, y + step / 2 - 8, linked ? "State: ready" : "State: link with PIN", linked ? CText() : CDim(), 1);
		}

		// status bar
		std::string hint;
		if(!active) hint = "";
		else if(profileIdx < np) hint = "X  Connect      Triangle  Edit / delete      Up / Down  Move";
		else hint = "X  Select      Left / Right  Move      Down  Profiles";
		UiText(20, 516, hint.c_str(), CDim(), 1);
		if(active && !status.empty() && status != "Who's playing?") UiTextCentered(486, status.c_str(), CAccent(), 1);
		UiText(905, 516, "v1.1", CDim(), 1);
		if(!gfx.fgReady) UiText(560, 516, "frame gen unavailable", CDim(), 1);
	}

	void DrawEditDialog()
	{
		Dialog(180, 50, 600, 440, editIdx >= 0 ? "Edit Profile" : "Add Profile");
		Field(204, 104, 552, "Name", editName, "Enter a name", editSel == 0);
		Field(204, 172, 552, "PSN Account ID", editAcc, "base64 (AbCdEfGhIjk=) or number", editSel == 1);
		Button(204, 244, 552, 38, "Fill in from this PS5's signed-in account", editSel == 2);
		Button(204, 296, editIdx >= 0 ? 268 : 552, 38, "Save", editSel == 3);
		if(editIdx >= 0) Button(488, 296, 268, 38, "Delete Profile", editSel == 4, editSel == 4 ? 0 : RGBA(255, 140, 120, 255));
		if(!editMsg.empty()) UiTextCentered(348, editMsg.c_str(), CAccent(), 1);
		UiTextWrapped(372, "Use the PSN account signed in on the PS5 you stream from. If it's the same account as this PS5, "
			"use Fill in. Otherwise type its account ID.", CDim(), 1, 540);
		UiTextCentered(464, "X  Select      O  Back", CDim(), 1);
	}

	void DrawKeyboardDialog()
	{
		Dialog(150, 40, 660, 460, kbTitle.c_str());
		UiRoundRect(174, 92, 612, 46, 4, RGBA(255, 255, 255, 30));
		UiRect(174, 136, 612, 2, CAccent());
		std::string shown = kbBuf + "_";
		UiText(188, 102, shown.c_str(), CText(), 2);
		const auto &rows = KbRows(kbMode);
		for(int r = 0; r <= (int)rows.size(); r++)
		{
			bool act = r == (int)rows.size();
			int len = KbRowLen(r);
			int w = act ? (kbMode == 1 ? 150 : 132) : 54, gap = 6;
			int total = len * (w + gap) - gap, x0 = 480 - total / 2, y = 156 + r * 52;
			for(int c = 0; c < len; c++)
			{
				bool sel = r == kbRow && c == kbCol;
				int x = x0 + c * (w + gap);
				bool on = act && KbActions(kbMode)[c] == "Shift" && kbShift;
				UiRoundRect(x, y, w, 44, 6, sel ? CAccent() : (on ? RGBA(0, 110, 170, 255) : CBtn()));
				std::string label;
				if(act) label = KbActions(kbMode)[c];
				else { char ch = rows[r][c]; if(kbShift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A'); label = std::string(1, ch); }
				int tw = UiTextWidth(label.c_str(), act ? 1 : 2);
				UiText(x + (w - tw) / 2, y + (act ? 14 : 8), label.c_str(), CText(), act ? 1 : 2);
			}
		}
		UiTextCentered(474, kbMode == 1 ? "X  Type      Square  Delete      Options  Done      O  Cancel"
			: "X  Type    Square  Delete    Triangle  Space    L1/R1  Shift    Options  Done    O  Cancel", CDim(), 1);
	}

	void DrawHelpDialog()
	{
		Dialog(90, 40, 780, 460, "Controls & Help");
		const char *lines[] = {
			"While streaming",
			"Hold Options + press D-pad Up: open / close the settings menu",
			"Hold Options + press D-pad Down: PS button on the streamed PS5",
			"Tap Options: Options in the game",
			"Settings menu: Up/Down choose, Left/Right change, X select, O close",
			"",
			"Setting up",
			"1. Console IP: the PS5 you stream from (on it: Settings > Network > Connection Status)",
			"2. + Add Profile: a name and the PSN account ID signed in on that PS5",
			"3. Pick the profile. On the other PS5 open Settings > System > Remote Play >",
			"    Link Device, and enter the PIN it shows here (only the first time)",
		};
		int y = 92;
		for(const char *l : lines)
		{
			bool head = !strcmp(l, "While streaming") || !strcmp(l, "Setting up");
			UiText(118, y, l, head ? CAccent() : CText(), 1);
			y += *l ? 28 : 14;
		}
		UiTextCentered(474, "O  Back", CDim(), 1);
	}

	void DrawPinDialog(const std::string &s)
	{
		Dialog(160, 100, 640, 330, "Link Profile");
		UiTextWrapped(150, s, CDim(), 1, 580);
		for(int i = 0; i < 8; i++)
		{
			int x = 480 - 4 * 68 + i * 68 + 4, y = 224;
			UiRoundRect(x, y, 60, 76, 6, i == pinCursor ? CAccent() : RGBA(255, 255, 255, 26));
			char d[2] = {(char)('0' + pinDigits[i]), 0};
			UiText(x + 20, y + 16, d, CText(), 3);
		}
		UiTextCentered(330, "Left/Right  Move      Up/Down  Change digit", CDim(), 1);
		UiTextCentered(352, "X  Register      O  Back", CDim(), 1);
	}

	void DrawStatusDialog(const std::string &s, State st)
	{
		const char *title = st == State::Error ? "Disconnected" : (st == State::Registering ? "Registering" : "Connecting");
		Dialog(220, 170, 520, 200, title);
		UiTextWrapped(222, s, st == State::Error ? RGBA(255, 150, 130, 255) : CText(), 1, 470);
		if(st == State::Error) UiTextCentered(336, "X  Retry      O  Back to profiles", CDim(), 1);
		else
		{
			int phase = (int)(NowUs() / 300000) % 4;
			for(int i = 0; i < 3; i++) UiRoundRect(462 + i * 14, 330, 8, 8, 4, i < phase ? CAccent() : RGBA(255, 255, 255, 50));
		}
	}

	void DrawUi()
	{
		UiClear();
		State st = state;
		uint32_t white = RGBA(235, 235, 240, 255), dim = RGBA(150, 150, 165, 255), accent = RGBA(80, 160, 255, 255);
		if(st != State::Streaming)
		{
			std::string s = GetStatus();
			DrawMainPage(s, st == State::ProfileSelect);
			if(st != State::ProfileSelect) UiRect(0, 0, 960, 540, RGBA(0, 0, 0, 140));
			if(st == State::EditProfile) DrawEditDialog();
			else if(st == State::Keyboard) DrawKeyboardDialog();
			else if(st == State::Help) DrawHelpDialog();
			else if(st == State::PinEntry) DrawPinDialog(s);
			else if(st != State::ProfileSelect) DrawStatusDialog(s, st);
		}
		else
		{
			if(set.fps)
			{
				char f[64];
				snprintf(f, sizeof(f), "%d fps  (%d shown)", fpsShown, fpsShownOut);
				UiRoundRect(UI_W / U - 190, 8, 182, 22, 11, RGBA(0, 0, 0, 150));
				UiText(UI_W / U - 184, 12, f, RGBA(120, 255, 140, 255), 1);
			}
			if(menuOpen)
			{
				UiRoundRect(560, 30, 380, 490, 10, RGBA(48, 48, 48, 235));
				UiText(580, 42, "Settings", RGBA(255, 255, 255, 255), 2);
				UiRect(580, 74, 340, 2, RGBA(0, 167, 255, 255));
				for(int i = 0; i < MENU_ITEMS; i++)
				{
					int y = 84 + i * 27;
					if(i == menuIdx) UiRoundRect(570, y - 5, 360, 26, 5, RGBA(0, 167, 255, 255));
					UiText(584, y, MenuLabel(i).c_str(), white, 1);
				}
				UiText(580, 462, "* applies on reconnect", dim, 1);
				UiText(580, 480, "Up/Down  Left/Right  X  O: close", dim, 1);
				UiText(580, 498, "Options+Up: menu   Options+Down: PS", RGBA(0, 167, 255, 255), 1);
			}
		}
		memcpy((uint8_t *)gfx.staging[gfx.slot].map + gfx.UiOffset(), g_ui.data(), g_ui.size() * 4);
	}

	PushConstants MakePC(bool video)
	{
		PushConstants pc{};
		pc.rect[0] = 0; pc.rect[1] = 0; pc.rect[2] = 1; pc.rect[3] = 1;
		pc.sharpen = set.sharpen / 10.0f;
		pc.deband = (float)set.deband;
		pc.restore = (float)set.restore;
		pc.boost = (float)set.boost;
		pc.hasVideo = video ? 1.0f : 0.0f;
		pc.uiOn = 1.0f;
		pc.time = (float)(NowUs() % 1000000) / 1000.0f;
		return pc;
	}

	// ---- main loop
	void Run()
	{
		fpsT0 = NowUs();
		uint64_t lastUiFrame = 0;
		bool uiUploadedSlot[2] = {false, false};
		while(running)
		{
			HandleInput();
			if(registDone.exchange(false) && registActive) { chiaki_regist_stop(&regist); chiaki_regist_fini(&regist); registActive = false; }
			if(wantConnect.exchange(false)) Connect();
			if(sessionQuit.exchange(false)) { /* stay in Error until the user picks */ }
			if(reconnectRequested) { reconnectRequested = false; inUseRetries = 0; StopSession(); Connect(); }
			if(retryAt && NowUs() >= retryAt) { retryAt = 0; StopSession(); Connect(); }
			if(disconnectRequested) { disconnectRequested = false; StopSession(); GoProfiles(); }

			// wait for a new video frame (or time out for UI)
			bool newFrame = false;
			uint32_t w = 0, h = 0;
			{
				std::unique_lock<std::mutex> l(frameMutex);
				if(frameSeq == shownSeq)
					frameCv.wait_for(l, std::chrono::milliseconds(4));
				if(frameSeq != shownSeq)
				{
					newFrame = true;
					shownSeq = frameSeq;
					w = frameW;
					h = frameH;
				}
			}

			uint64_t now = NowUs();
			if(now - fpsT0 >= 1000000)
			{
				fpsShown = fpsFrames;
				fpsShownOut = fpsPresents;
				fpsFrames = fpsPresents = 0;
				fpsT0 = now;
				if(set.fps && state == State::Streaming) uiDirty = true;
			}

			bool streaming = state == State::Streaming;
			if(!newFrame)
			{
				// redraw the UI at most ~30 times a second when something changed
				if(!uiDirty || now - lastUiFrame < 33000) continue;
			}
			lastUiFrame = now;

			VkCommandBuffer cmd = gfx.Begin();
			int s = gfx.slot;
			if(newFrame)
			{
				gfx.EnsureVideoSize(w, h);
				gfx.InitVideoLayouts(cmd);
				std::lock_guard<std::mutex> l(frameMutex);
				memcpy(gfx.staging[s].map, frameBuf.data(), (size_t)w * h * 3 / 2);
				haveVideo = true;
				fpsFrames++;
			}
			{
				// the connecting dots move: redraw a few times a second while connecting
				State cs = state;
				static uint64_t lastAnim = 0;
				uint64_t tn = NowUs();
				if((cs == State::Connecting || cs == State::Registering) && tn - lastAnim > 300000) { lastAnim = tn; uiDirty = true; }
			}
			bool uploadUi = uiDirty || !uiUploadedSlot[s];
			if(uiDirty) { DrawUi(); uiUploadedSlot[0] = uiUploadedSlot[1] = false; uiDirty = false; }
			else if(uploadUi) DrawUi();
			uiUploadedSlot[s] = true;

			gfx.Draw(cmd, newFrame, uploadUi, MakePC(haveVideo && streaming));

			bool generated = false;
			if(newFrame && streaming && activeFg && gfx.fgReady)
			{
				gfx.CopyIntoFg(cmd);
				gfx.fg.set_generated(1);
				generated = gfx.fg.record(cmd, fgReset);
				fgReset = false;
				Renderer::Barrier(cmd);
			}
			if(generated)
			{
				// in-between frame first, then the real one
				gfx.EndAndPresent(cmd, gfx.fg.output_image());
				fpsPresents++;
				VkCommandBuffer cmd2 = gfx.Begin();
				gfx.EndAndPresent(cmd2, gfx.frame.image);
				fpsPresents++;
				gfx.fg.advance();
			}
			else
			{
				gfx.EndAndPresent(cmd, gfx.frame.image);
				fpsPresents++;
				if(newFrame && streaming && activeFg && gfx.fgReady) gfx.fg.advance();
			}
		}
	}
};

static App g_app;

int main(void)
{
	if(!g_app.Init())
	{
		AppLog("startup failed; see %s/log.txt", DATA_DIR);
		for(;;) sleep(1);
	}
	g_app.Run();
	return 0;
}
