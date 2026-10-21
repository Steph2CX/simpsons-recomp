// Decodeur materiel XMA de la Xbox 360, emule avec FFmpeg.
//
// Chaque contexte a deux tampons d'entree (paquets XMA de 2 Ko) et un tampon de sortie circulaire
// (blocs de 256 octets, PCM 16 bits big-endian entrelace). Un thread decode les paquets quand le jeu
// a libere de la place dans la sortie : la consommation suit donc le rythme de lecture du jeu.
// L'etat est aussi recopie dans la structure materielle de 64 octets (disposition de Xenia).
#include "runtime.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

constexpr uint32_t XMA_PACKET_SIZE = 2048;
constexpr uint32_t XMA_OUTPUT_BLOCK_SIZE = 256;

struct XmaInputBuffer
{
    uint32_t address = 0;
    uint32_t packetCount = 0;
    bool valid = false;
};

struct XmaContext
{
    uint32_t guest = 0;
    XmaInputBuffer input[2];
    uint32_t currentInput = 0;
    uint32_t nextPacket = 0;

    uint32_t outputAddress = 0;
    uint32_t outputBlocks = 0;
    uint32_t outputWriteBlock = 0;
    uint32_t outputReadBlock = 0;
    bool outputValid = false;

    uint32_t channels = 2;
    uint32_t sampleRate = 48000;
    bool enabled = false;

    // FFmpeg decoder (about 6 MB): opened when data arrives, closed after IDLE_DECODER of silence. The game keeps
    // up to 256 contexts enabled while only a few play: one decoder each took 1.4 GB (the Apple TV allows 2 GB)
    AVCodecContext* decoder = nullptr;
    bool decoderWanted = false; // initialized: open the decoder when packets come
    std::chrono::steady_clock::time_point lastData;
    std::deque<uint8_t> pending; // decoded PCM (s16 big-endian, interleaved) waiting for output space
};

static std::mutex g_xmaMutex;
static std::unordered_map<uint32_t, XmaContext*> g_xmaContexts;
static bool g_xmaThreadStarted = false;

static XmaContext* FindContext(uint32_t guest)
{
    auto it = g_xmaContexts.find(guest);
    return it != g_xmaContexts.end() ? it->second : nullptr;
}

// Mirror the state into the 64-byte hardware context (bitfields of big-endian dwords)
static void SyncGuestContext(const XmaContext& c)
{
    uint32_t dword0 = (c.input[0].packetCount & 0xFFF) | (uint32_t(c.input[0].valid) << 20) |
        (uint32_t(c.input[1].valid) << 21) | ((c.outputBlocks & 0x1F) << 22) | ((c.outputWriteBlock & 0x1F) << 27);
    static const uint32_t rateIndex[] = { 24000, 32000, 44100, 48000 };
    uint32_t rate = 3;
    for (uint32_t i = 0; i < 4; i++)
    {
        if (rateIndex[i] == c.sampleRate)
            rate = i;
    }
    uint32_t dword1 = (c.input[1].packetCount & 0xFFF) | (rate << 28) | (uint32_t(c.channels == 2) << 30) |
        (uint32_t(c.outputValid) << 31);
    StoreBE32(c.guest + 0x00, dword0);
    StoreBE32(c.guest + 0x04, dword1);
    StoreBE32(c.guest + 0x14, c.input[0].address ? VirtualToPhysical(c.input[0].address) : 0);
    StoreBE32(c.guest + 0x18, c.input[1].address ? VirtualToPhysical(c.input[1].address) : 0);
    StoreBE32(c.guest + 0x1C, c.outputAddress ? VirtualToPhysical(c.outputAddress) : 0);
    StoreBE32(c.guest + 0x24, c.outputReadBlock & 0x1F);
}

constexpr auto IDLE_DECODER = std::chrono::seconds(2);

static void OpenDecoder(XmaContext& c)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    c.decoder = avcodec_alloc_context3(codec);
    av_channel_layout_default(&c.decoder->ch_layout, int(c.channels));
    c.decoder->sample_rate = int(c.sampleRate);
    // 34 bytes = XMA2WAVEFORMATEX: FFmpeg then decodes a single 1 or 2 channel stream
    c.decoder->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    c.decoder->extradata_size = 34;
    c.decoder->extradata[0] = 1; // NumStreams (little-endian)
    c.decoder->extradata[2] = c.channels == 2 ? 0x03 : 0x04; // ChannelMask: front left+right, or center
    if (avcodec_open2(c.decoder, codec, nullptr) < 0)
    {
        fprintf(stderr, "[xma] impossible d'ouvrir le decodeur FFmpeg\n");
        avcodec_free_context(&c.decoder);
        c.decoderWanted = false;
    }
}

// New stream: the decoder starts again from scratch, when the first packet comes
static void ResetDecoder(XmaContext& c)
{
    if (c.decoder != nullptr)
        avcodec_free_context(&c.decoder);
    c.decoderWanted = true;
    c.pending.clear();
}

static void DecodePacket(XmaContext& c, const uint8_t* packet)
{
    if (c.decoder == nullptr && c.decoderWanted)
        OpenDecoder(c);
    if (c.decoder == nullptr)
        return;
    c.lastData = std::chrono::steady_clock::now();
    static const bool trace = getenv("SIMPSONS_TRACE_AUDIO") != nullptr;
    static int traced = 0;
    size_t before = c.pending.size();

    AVPacket* av = av_packet_alloc();
    av_new_packet(av, XMA_PACKET_SIZE);
    memcpy(av->data, packet, XMA_PACKET_SIZE);
    if (avcodec_send_packet(c.decoder, av) == 0)
    {
        AVFrame* frame = av_frame_alloc();
        while (avcodec_receive_frame(c.decoder, frame) == 0)
        {
            uint32_t channels = uint32_t(frame->ch_layout.nb_channels);
            for (int i = 0; i < frame->nb_samples; i++)
            {
                for (uint32_t ch = 0; ch < c.channels; ch++)
                {
                    float value = reinterpret_cast<float*>(frame->data[std::min(ch, channels - 1)])[i];
                    int32_t sample = int32_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
                    c.pending.push_back(uint8_t(sample >> 8));
                    c.pending.push_back(uint8_t(sample));
                }
            }
        }
        av_frame_free(&frame);
    }
    av_packet_free(&av);

    // SIMPSONS_DUMP_XMA: append each context's decoded PCM to xma_<context>.raw (s16 big-endian)
    static const bool dump = getenv("SIMPSONS_DUMP_XMA") != nullptr;
    if (dump && c.pending.size() > before)
    {
        char name[64];
        snprintf(name, sizeof(name), "recomp/xma_%08X_%uch.raw", c.guest, c.channels);
        if (FILE* f = fopen(name, "ab"))
        {
            std::vector<uint8_t> bytes(c.pending.begin() + before, c.pending.end());
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    }

    if (trace && traced < 12)
    {
        traced++;
        int peak = 0;
        for (size_t i = before; i + 1 < c.pending.size(); i += 2)
            peak = std::max(peak, std::abs(int(int16_t((c.pending[i] << 8) | c.pending[i + 1]))));
        fprintf(stderr, "[xma] contexte 0x%X : paquet %02X %02X %02X %02X -> %zu octets PCM, crete %d\n", c.guest,
            packet[0], packet[1], packet[2], packet[3], c.pending.size() - before, peak);
    }
}

// Returns true if some work was done
static bool StepContext(XmaContext& c)
{
    if (!c.enabled || !c.outputValid || c.outputBlocks == 0)
        return false;

    bool worked = false;
    uint32_t capacity = c.outputBlocks;
    uint32_t used = (c.outputWriteBlock + capacity - c.outputReadBlock) % capacity;
    uint32_t freeBlocks = capacity - 1 - used; // one block stays free so that full != empty

    // Fill the output ring with already decoded PCM
    while (freeBlocks > 0 && c.pending.size() >= XMA_OUTPUT_BLOCK_SIZE)
    {
        uint8_t* block = static_cast<uint8_t*>(GuestToHost(c.outputAddress + c.outputWriteBlock * XMA_OUTPUT_BLOCK_SIZE));
        std::copy_n(c.pending.begin(), XMA_OUTPUT_BLOCK_SIZE, block);
        c.pending.erase(c.pending.begin(), c.pending.begin() + XMA_OUTPUT_BLOCK_SIZE);
        c.outputWriteBlock = (c.outputWriteBlock + 1) % capacity;
        freeBlocks--;
        worked = true;
    }

    // Decode one more packet when the pending PCM runs low
    if (c.pending.size() < capacity * XMA_OUTPUT_BLOCK_SIZE)
    {
        auto& buffer = c.input[c.currentInput];
        if (buffer.valid && buffer.address != 0)
        {
            DecodePacket(c, static_cast<uint8_t*>(GuestToHost(buffer.address + c.nextPacket * XMA_PACKET_SIZE)));
            worked = true;
            if (++c.nextPacket >= buffer.packetCount)
            {
                // Buffer consumed: hand it back to the game and switch to the other one
                buffer.valid = false;
                c.nextPacket = 0;
                c.currentInput ^= 1;
            }
        }
        else if (c.input[c.currentInput ^ 1].valid)
        {
            c.currentInput ^= 1;
            c.nextPacket = 0;
            worked = true;
        }
    }

    if (worked)
        SyncGuestContext(c);
    return worked;
}

static void XmaThread()
{
    static const bool trace = getenv("SIMPSONS_TRACE_AUDIO") != nullptr;
    auto lastReport = std::chrono::steady_clock::now(), lastCleanup = lastReport;
    while (true)
    {
        bool worked = false;
        {
            std::lock_guard lock(g_xmaMutex);
            for (auto& [guest, context] : g_xmaContexts)
                worked |= StepContext(*context);
            auto now = std::chrono::steady_clock::now();
            if (now - lastCleanup > std::chrono::seconds(1))
            {
                lastCleanup = now;
                for (auto& [guest, c] : g_xmaContexts)
                    if (c->decoder != nullptr && !c->input[0].valid && !c->input[1].valid && c->pending.empty() &&
                        now - c->lastData > IDLE_DECODER)
                        avcodec_free_context(&c->decoder); // decoderWanted stays: opened again with the next packet
            }
            if (trace && now - lastReport > std::chrono::seconds(10))
            {
                lastReport = now;
                size_t decoders = 0, enabled = 0, busy = 0;
                for (auto& [guest, c] : g_xmaContexts)
                {
                    decoders += c->decoder != nullptr;
                    enabled += c->enabled;
                    busy += c->input[0].valid || c->input[1].valid;
                }
                fprintf(stderr, "[xma] %zu contextes, %zu decodeurs, %zu actifs, %zu avec des donnees\n", g_xmaContexts.size(),
                    decoders, enabled, busy);
            }
        }
        if (!worked)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ---------------------------------------------------------------------------
// Kernel API

KERNEL_FUNC(XMACreateContext)
{
    // (DWORD* Context) - 64-byte hardware structure in physical memory
    LOG_CALL(XMACreateContext);
    uint32_t guest = GuestAllocPhysical(0x40, 0x40, 0x20000000);
    std::lock_guard lock(g_xmaMutex);
    auto* context = new XmaContext();
    context->guest = guest;
    g_xmaContexts[guest] = context;
    if (!g_xmaThreadStarted)
    {
        g_xmaThreadStarted = true;
        std::thread(XmaThread).detach();
    }
    StoreBE32(ctx.r3.u32, guest);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(XMAReleaseContext)
{
    LOG_CALL(XMAReleaseContext);
    std::lock_guard lock(g_xmaMutex);
    if (auto* c = FindContext(ctx.r3.u32))
    {
        if (c->decoder != nullptr)
            avcodec_free_context(&c->decoder);
        g_xmaContexts.erase(ctx.r3.u32);
        delete c;
    }
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(XMAInitializeContext)
{
    // (VOID* Context, XMA_CONTEXT_INIT* Init): input buffers 0/1 and packet counts, input read offset,
    // output buffer and block count, work buffer, subframe count, channel count - 1, sample rate index
    LOG_CALL(XMAInitializeContext);
    uint32_t init = ctx.r4.u32;
    std::lock_guard lock(g_xmaMutex);
    auto* c = FindContext(ctx.r3.u32);
    if (c == nullptr)
    {
        ctx.r3.u64 = 0;
        return;
    }
    memset(GuestToHost(c->guest), 0, 0x40);
    c->input[0] = { LoadBE32(init + 0x00), LoadBE32(init + 0x04), false };
    c->input[1] = { LoadBE32(init + 0x08), LoadBE32(init + 0x0C), false };
    c->currentInput = 0;
    c->nextPacket = 0;
    c->outputAddress = LoadBE32(init + 0x14);
    c->outputBlocks = LoadBE32(init + 0x18);
    c->outputWriteBlock = 0;
    c->outputReadBlock = 0;
    c->outputValid = false;
    c->channels = LoadBE32(init + 0x24) + 1;
    static const uint32_t rates[] = { 24000, 32000, 44100, 48000 };
    c->sampleRate = rates[LoadBE32(init + 0x28) & 3];
    c->enabled = false;
    ResetDecoder(*c);
    SyncGuestContext(*c);
    ctx.r3.u64 = 0;
}

#define XMA_CONTEXT_FUNC(name, body)                      \
    KERNEL_FUNC(name)                                     \
    {                                                     \
        LOG_CALL(name);                                   \
        std::lock_guard lock(g_xmaMutex);                 \
        auto* c = FindContext(ctx.r3.u32);                \
        uint64_t result = 0;                              \
        if (c != nullptr)                                 \
        {                                                 \
            body;                                         \
            SyncGuestContext(*c);                         \
        }                                                 \
        ctx.r3.u64 = result;                              \
    }

XMA_CONTEXT_FUNC(XMAEnableContext, c->enabled = true)
XMA_CONTEXT_FUNC(XMADisableContext, c->enabled = false)
XMA_CONTEXT_FUNC(XMASetInputBuffer0, c->input[0].address = ctx.r4.u32; c->input[0].packetCount = ctx.r5.u32)
XMA_CONTEXT_FUNC(XMASetInputBuffer1, c->input[1].address = ctx.r4.u32; c->input[1].packetCount = ctx.r5.u32)
XMA_CONTEXT_FUNC(XMASetInputBuffer0Valid, c->input[0].valid = true)
XMA_CONTEXT_FUNC(XMASetInputBuffer1Valid, c->input[1].valid = true)
XMA_CONTEXT_FUNC(XMAIsInputBuffer0Valid, result = c->input[0].valid)
XMA_CONTEXT_FUNC(XMAIsInputBuffer1Valid, result = c->input[1].valid)
XMA_CONTEXT_FUNC(XMASetOutputBufferValid, c->outputValid = true)
XMA_CONTEXT_FUNC(XMAIsOutputBufferValid, result = c->outputValid)
XMA_CONTEXT_FUNC(XMAGetOutputBufferWriteOffset, result = c->outputWriteBlock)
XMA_CONTEXT_FUNC(XMAGetOutputBufferReadOffset, result = c->outputReadBlock)
XMA_CONTEXT_FUNC(XMASetOutputBufferReadOffset, c->outputReadBlock = ctx.r4.u32 % std::max<uint32_t>(c->outputBlocks, 1))

// Debugging (SIMPSONS_SURVEILLE): which context field holds this physical address, if any
const char* XmaDescribePhysical(uint32_t physical)
{
    static thread_local char text[96];
    std::lock_guard lock(g_xmaMutex);
    for (auto& [guest, c] : g_xmaContexts)
    {
        const uint32_t fields[3] = { c->input[0].address, c->input[1].address, c->outputAddress };
        static const char* names[3] = { "entree 0", "entree 1", "sortie" };
        for (int i = 0; i < 3; i++)
        {
            if (fields[i] && VirtualToPhysical(fields[i]) == physical)
            {
                snprintf(text, sizeof(text), "contexte XMA 0x%08X, %s", guest, names[i]);
                return text;
            }
        }
    }
    return nullptr;
}
