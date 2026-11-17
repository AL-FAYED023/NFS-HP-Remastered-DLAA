// NFS Hot Pursuit Remastered - DLAA 모드 (ASI)  v3
//
// 지금까지 확인된 사실
//  - 게임 D3D11 컨텍스트는 객체마다 vtable 복사본을 가짐 -> Present 에서 실제 객체를 찾아 패치
//  - d3d11.dll 이 Draw/DrawIndexed/Map 슬롯을 동적으로 바꿔 끼움 -> 이 함수들은 후킹 불가
//  - PSSetShader 슬롯은 안정적 -> PSSetShaderResources / PSSetConstantBuffers 도 같은 계열
//
// 동작
//  1) PSSetShaderResources 로 TAA 입력 4개(t0 색상, t1 히스토리, t2 속도 RG16, t3 깊이)가
//     연결되는 순간을 감지해 버퍼들을 기억
//  2) 다음 패스가 TAA 출력 텍스처(히스토리가 아닌 쪽)를 SRV 로 연결하는 순간 = TAA 완료
//     -> 그 텍스처를 DLAA 결과로 덮어씀
//  3) 상수버퍼(ClipToPrevClip, 지터)는 GPU 에서 복사해 모션벡터 계산에 사용,
//     지터는 2프레임 늦게 읽어 Halton(2,3) 순서로 현재 값을 예측

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// 1 = 진단 메뉴 표시 (개발용), 0 = 배포용
#ifndef NFS_DLAA_DIAG
#define NFS_DLAA_DIAG 0
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <mutex>
#include <string>
#include <algorithm>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern "C" int Vtbl_PSSetShader(void);
extern "C" int Vtbl_PSSetShaderResources(void);
extern "C" int Vtbl_PSSetConstantBuffers(void);
extern "C" int Vtbl_Present(void);
extern "C" int Vtbl_Present1(void);

// ================================================================== 로그 / 설정
static FILE*        g_log = nullptr;
static std::wstring g_dir, g_ini;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d][pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

struct Config {
    bool enabled = true;
    int  preset = 11;   // K
    int  keyToggle = VK_F9;
    int  keyMenu = VK_F8;
    // 진단용 (임시): 지터 부호 0~3, 모션벡터에 지터 포함 여부
    int  diagJitterSign = 0;
    bool diagMVJittered = false;
    int  diagVelMode = 0;   // 속도 버퍼 처리 (진단)
    int  minWidth = 1280;
    bool verbose = false;
    int  lang = 0;      // 0 = English (default), 1 = Korean
    float sharpness = 0.0f;   // 0 = off, 0..1 adaptive sharpening (CAS)
    bool noCrashCams = false; // skip the wreck (crash) cameras
} g_cfg;

// Disable crash cams state (used by the menu, code further below)
static volatile LONG g_ccState = 0;      // 0 = not scanned, 1 = scanning, 2 = ready, 3 = not available
static bool          g_ccApplied = false;

// UI strings: pick the current language
static const char* TR(const char* en, const char* ko) { return g_cfg.lang == 1 ? ko : en; }

static void LoadConfig()
{
    const wchar_t* s = L"DLAA"; const wchar_t* f = g_ini.c_str();
    g_cfg.enabled    = GetPrivateProfileIntW(s, L"Enabled", 1, f) != 0;
    g_cfg.preset     = GetPrivateProfileIntW(s, L"Preset", 11, f);
    switch (g_cfg.preset) { case 0: case 5: case 6: case 10: case 11: case 12: case 13: break; default: g_cfg.preset = 11; }   // valid SDK 310.9 presets only
    g_cfg.noCrashCams = GetPrivateProfileIntW(s, L"NoCrashCams", 0, f) != 0;
    g_cfg.sharpness  = std::min(100, std::max(0, (int)GetPrivateProfileIntW(s, L"Sharpness", 0, f))) / 100.0f;
    g_cfg.keyToggle  = GetPrivateProfileIntW(s, L"ToggleKey", VK_F9, f);
    g_cfg.keyMenu    = GetPrivateProfileIntW(s, L"MenuKey", VK_F8, f);
#if NFS_DLAA_DIAG
    g_cfg.diagJitterSign = GetPrivateProfileIntW(s, L"DiagJitterSign", 0, f) & 3;
    g_cfg.diagMVJittered = GetPrivateProfileIntW(s, L"DiagMVJittered", 0, f) != 0;
    g_cfg.diagVelMode    = GetPrivateProfileIntW(s, L"DiagVelMode", 0, f) & 3;
#endif
    g_cfg.minWidth   = GetPrivateProfileIntW(s, L"MinWidth", 1280, f);
    g_cfg.verbose    = GetPrivateProfileIntW(s, L"VerboseLog", 0, f) != 0;
    wchar_t lg[16] = {};
    GetPrivateProfileStringW(s, L"Language", L"en", lg, 16, f);
    g_cfg.lang = (_wcsicmp(lg, L"ko") == 0 || _wcsicmp(lg, L"kr") == 0) ? 1 : 0;
}

template<class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
template<class T> static void Assign(T*& dst, T* src) { if (src) src->AddRef(); SafeRelease(dst); dst = src; }

static void LogModuleOf(const char* what, void* addr)
{
    HMODULE m = nullptr; wchar_t name[MAX_PATH] = L"(heap/unknown)";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) && m)
        GetModuleFileNameW(m, name, MAX_PATH);
    char n8[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, name, -1, n8, sizeof(n8), nullptr, nullptr);
    Log("%s %p -> %s", what, addr, n8);
}

// ================================================================== 훅 테이블
typedef void (STDMETHODCALLTYPE* PFN_PSSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef void (STDMETHODCALLTYPE* PFN_PSSetSRV)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void (STDMETHODCALLTYPE* PFN_PSSetCB)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

struct HookedCtx { void** vt = nullptr; PFN_PSSetShader PSSetShader = nullptr; PFN_PSSetSRV PSSetSRV = nullptr; PFN_PSSetCB PSSetCB = nullptr; };
struct HookedSC  { void** vt = nullptr; PFN_Present Present = nullptr; PFN_Present1 Present1 = nullptr; };
static HookedCtx  g_ctxVt[16]; static int g_ctxVtCount = 0;
static HookedSC   g_scVt[4];   static int g_scVtCount = 0;
static std::mutex g_vtLock;

static inline HookedCtx* FindCtx(void* o) { void** vt = *(void***)o; for (int i = 0; i < g_ctxVtCount; ++i) if (g_ctxVt[i].vt == vt) return &g_ctxVt[i]; return nullptr; }
static inline HookedSC*  FindSC(void* o)  { void** vt = *(void***)o; for (int i = 0; i < g_scVtCount;  ++i) if (g_scVt[i].vt  == vt) return &g_scVt[i];  return nullptr; }

static void WriteSlot(void** vt, int idx, void* fn)
{
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

// ================================================================== 상태
static ID3D11PixelShader* g_curPS = nullptr;
static bool  g_inDLAA = false;
static bool  g_fatal = false;

// TAA 패스에서 기억한 것
static ID3D11PixelShader*        g_taaPS = nullptr;     // 비교용 (참조 안 잡음)
static ID3D11Texture2D*          g_colorTex = nullptr;  // t0
static ID3D11ShaderResourceView* g_velSRV = nullptr;    // t2
static ID3D11ShaderResourceView* g_depSRV = nullptr;    // t3
static ID3D11Texture2D*          g_curHist = nullptr;   // 이번 프레임 t1 (=입력 히스토리)
static ID3D11Texture2D*          g_histA = nullptr;     // 핑퐁 텍스처 두 개
static ID3D11Texture2D*          g_histB = nullptr;
static ID3D11Buffer*             g_taaCB = nullptr;     // TAA 셰이더의 b0
static bool g_pendingTAA = false;
static bool g_doneThisFrame = false;

// 통계
static volatile LONG g_statPS = 0, g_statSRV = 0, g_statTAABind = 0, g_statTrigger = 0, g_statMissed = 0;

// DLAA 리소스
static ID3D11Device*            g_dev = nullptr;
static ID3DDeviceContextState*  g_ourState = nullptr;
static ID3D11ComputeShader*     g_cs = nullptr;
static ID3D11Buffer*            g_cbCopy = nullptr;
static ID3D11Buffer*            g_modeCB = nullptr;   // 진단용 모드 (b1)
static ID3D11Buffer*            g_cbStage[4] = {};
static uint64_t                 g_cbStageFrame[4] = {};
static bool                     g_cbStageUsed[4] = {};
static ID3D11Texture2D*         g_mvTex = nullptr;  static ID3D11UnorderedAccessView* g_mvUAV = nullptr;
static ID3D11Texture2D*         g_depTex = nullptr; static ID3D11UnorderedAccessView* g_depUAV = nullptr;
static ID3D11Texture2D*         g_outTex = nullptr;
static ID3D11ShaderResourceView* g_outSRV = nullptr;   // sharpening input
static ID3D11Texture2D*         g_shTex = nullptr;  static ID3D11UnorderedAccessView* g_shUAV = nullptr;
static ID3D11ComputeShader*     g_casCS = nullptr;
static ID3D11Buffer*            g_casCB = nullptr;
static ID3D11Texture2D*         g_colCopy = nullptr;
static UINT g_w = 0, g_h = 0;
static DXGI_FORMAT g_outFmt = DXGI_FORMAT_UNKNOWN, g_colFmt = DXGI_FORMAT_UNKNOWN;

static bool                 g_ngxInit = false;
static NVSDK_NGX_Parameter* g_ngxParams = nullptr;
static NVSDK_NGX_Handle*    g_dlss = nullptr;
static bool     g_resetNext = true;
static uint64_t g_frame = 0;       // DLAA 실행 횟수
static uint64_t g_gameFrame = 0;   // 게임 프레임 번호 (Present 기준, 상태 표시용)
static uint64_t g_taaFrame = 0;    // TAA 실행 횟수 (지터 순서 추적 기준)
static uint64_t g_lastDLAAGameFrame = 0;
static bool     g_presetDirty = false;  // 프리셋 변경 -> DLSS 기능 재생성

// ================================================================== 유틸
static DXGI_FORMAT TypedOf(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
static bool Is1010102(DXGI_FORMAT f) { return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS; }
static bool IsRG16(DXGI_FORMAT f)    { return f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_TYPELESS; }
static bool IsDepthSRV(DXGI_FORMAT f){ return f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_R32_FLOAT; }

static ID3D11Texture2D* TexOf(ID3D11View* v)   // AddRef 된 텍스처 반환
{
    if (!v) return nullptr;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return nullptr;
    ID3D11Texture2D* t = nullptr;
    r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
    r->Release();
    return t;
}
static DXGI_FORMAT SrvFmt(ID3D11ShaderResourceView* v) { D3D11_SHADER_RESOURCE_VIEW_DESC d; v->GetDesc(&d); return d.Format; }

// ================================================================== Halton 지터 예측
static double Halton(int i, int b) { double f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i /= b; } return r; }

static int g_jIdxMin = 1 << 30, g_jIdxMax = -1, g_jPeriod = 0;
static int g_jLastIdx = -1; static uint64_t g_jLastFrame = 0; static bool g_jHave = false;
static int g_jPrevObserved = -1;
static int g_jChecks = 0, g_jHits = 0;
static float g_jPredX[8] = {}, g_jPredY[8] = {};   // 프레임별 예측값 (검증용)

static int FindHaltonIndex(float jx, float jy)   // jx,jy: 픽셀, 범위 -0.5~0.5
{
    int best = -1; double bestErr = 1e9;
    for (int i = 0; i <= 1024; ++i) {
        double ex = Halton(i, 2) - 0.5 - jx, ey = Halton(i, 3) - 0.5 - jy;
        double e = ex * ex + ey * ey;
        if (e < bestErr) { bestErr = e; best = i; }
    }
    return bestErr < 1e-5 ? best : -1;
}

static void ObserveJitter(uint64_t frame, const float* cb, UINT w, UINT h)
{
    float jx = cb[8] * w * 0.5f, jy = -cb[9] * h * 0.5f;
    int idx = FindHaltonIndex(jx, jy);
    if (frame + 8 > g_taaFrame) {   // 그 프레임에 사용했던 예측값과 비교
        float px = g_jPredX[frame & 7], py = g_jPredY[frame & 7];
        ++g_jChecks;
        if (fabsf(px - jx) < 1e-3f && fabsf(py - jy) < 1e-3f) ++g_jHits;
        else {
            static int missLogs = 0;
            if (g_jPeriod > 0 && missLogs < 30) {
                ++missLogs;
                Log("Jitter prediction mismatch: TAA frame %llu predicted Halton #%d / actual #%d (jx %.4f jy %.4f)",
                    (unsigned long long)frame, FindHaltonIndex(px, py), idx, jx, jy);
            }
        }
    }
    if (g_frame < 40 || g_cfg.verbose)
        Log("Jitter observed: frame %llu jx %.4f jy %.4f -> Halton #%d (cycle %d)", (unsigned long long)frame, jx, jy, idx, g_jPeriod);
    if (idx < 0) return;
    if (g_jPrevObserved >= 0 && idx < g_jPrevObserved) {
        int p = g_jIdxMax - g_jIdxMin + 1;
        if (p != g_jPeriod) { g_jPeriod = p; Log("Jitter cycle confirmed: %d (index %d~%d)", p, g_jIdxMin, g_jIdxMax); }
    }
    g_jPrevObserved = idx;
    if (idx < g_jIdxMin) g_jIdxMin = idx;
    if (idx > g_jIdxMax) g_jIdxMax = idx;
    g_jLastIdx = idx; g_jLastFrame = frame; g_jHave = true;
}

static bool PredictJitter(uint64_t frame, float* jx, float* jy)
{
    if (!g_jHave) { *jx = *jy = 0; return false; }
    int64_t d = (int64_t)(frame - g_jLastFrame);
    int idx = g_jLastIdx + (int)d;
    if (g_jPeriod > 0) idx = g_jIdxMin + (int)(((int64_t)(g_jLastIdx - g_jIdxMin) + d) % g_jPeriod);
    *jx = (float)(Halton(idx, 2) - 0.5);
    *jy = (float)(Halton(idx, 3) - 0.5);
    return true;
}

// ================================================================== 컴퓨트 셰이더
static const char* kCS = R"(
cbuffer CB : register(b0) { float4 P[4]; float4 C[4]; };
cbuffer MODE : register(b1) { uint velMode; uint3 pad_; };   // 진단: 0 그대로, 1 지터차 빼기, 2 지터차 더하기, 3 속도버퍼 무시
Texture2D<float4> Vel : register(t0);
Texture2D<float4> Dep : register(t1);
RWTexture2D<float2> OutMV : register(u0);
RWTexture2D<float>  OutDepth : register(u1);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)P[0].z || id.y >= (uint)P[0].w) return;
    float2 uv = (id.xy + 0.5) * P[0].xy;
    float  d  = Dep.Load(int3(id.xy, 0)).x;
    float2 v  = Vel.Load(int3(id.xy, 0)).xy;
    float2 cc = (uv - 0.5) * float2(2, -2);         // 카메라 재투영 (지터 없음 확인됨)
    float3 r  = cc.y * C[1].xyw + cc.x * C[0].xyw + d * C[2].xyw + C[3].xyw;
    float2 camOff = (r.xy / r.z - cc) * float2(0.5, -0.5);
    float2 jd = (P[2].zw - P[2].xy) * float2(0.5, -0.5);   // 이전 지터 - 현재 지터 (UV)
    float2 off = camOff;
    if ((v.x > 0 || v.y > 0) && velMode != 3) {
        off = 1.0 - 2.0 * v;                       // 오브젝트 속도 (UV, 현재->이전)
        if (velMode == 1) off -= jd;
        if (velMode == 2) off += jd;
    }
    OutMV[id.xy]    = off * P[0].zw;
    OutDepth[id.xy] = d;
}
)";

// Adaptive sharpening (same idea as AMD CAS: strong where local contrast is low, weak on edges that are already sharp)
static const char* kCAS = R"(
cbuffer S : register(b0) { float sharp; float3 pad_; };
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; Src.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = id.xy, m = int2(w - 1, h - 1);
    float3 a = Src[clamp(p + int2(-1,-1), 0, m)].rgb, b = Src[clamp(p + int2(0,-1), 0, m)].rgb, c = Src[clamp(p + int2(1,-1), 0, m)].rgb;
    float3 d = Src[clamp(p + int2(-1, 0), 0, m)].rgb; float4 e4 = Src[p]; float3 e = e4.rgb; float3 f = Src[clamp(p + int2(1, 0), 0, m)].rgb;
    float3 g = Src[clamp(p + int2(-1, 1), 0, m)].rgb, hh = Src[clamp(p + int2(0, 1), 0, m)].rgb, i = Src[clamp(p + int2(1, 1), 0, m)].rgb;
    float3 mn = min(min(min(d, e), min(f, b)), hh);  mn += min(mn, min(min(a, c), min(g, i)));
    float3 mx = max(max(max(d, e), max(f, b)), hh);  mx += max(mx, max(max(a, c), max(g, i)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, sharp));
    float3 o = saturate((b * wgt + d * wgt + f * wgt + hh * wgt + e) / (1.0 + 4.0 * wgt));
    Dst[p] = float4(o, e4.a);
}
)";

static bool CreateCAS(ID3D11Device* dev)
{
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kCAS, strlen(kCAS), "dlaa_cas", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) { Log("Sharpening shader compile failed 0x%08X %s", hr, err ? (const char*)err->GetBufferPointer() : ""); SafeRelease(err); return false; }
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_casCS);
    code->Release(); SafeRelease(err);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &g_casCB);
    if (FAILED(hr)) { Log("Sharpening shader creation failed 0x%08X", hr); return false; }
    Log("Sharpening shader ready");
    return true;
}

static bool CreateCS(ID3D11Device* dev)
{
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kCS, strlen(kCS), "dlaa_mv", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) { Log("Compute shader compile failed 0x%08X %s", hr, err ? (const char*)err->GetBufferPointer() : ""); SafeRelease(err); return false; }
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
    code->Release(); SafeRelease(err);
    if (FAILED(hr)) { Log("CreateComputeShader failed 0x%08X", hr); return false; }
    return true;
}

static bool CreateStateObject(ID3D11Device* dev)
{
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { Log("ID3D11Device1 not supported"); return false; }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = dev1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_ourState);
    dev1->Release();
    if (FAILED(hr)) { Log("CreateDeviceContextState failed 0x%08X", hr); return false; }
    return true;
}

static ID3D11Texture2D* MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr)) { Log("CreateTexture2D(%ux%u fmt %d) failed 0x%08X", w, h, (int)f, hr); return nullptr; }
    return t;
}

static bool InitNGX(ID3D11Device* dev)
{
    if (g_ngxInit) return true;
    const wchar_t* paths[] = { g_dir.c_str() };
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("7d1c3b8e-5a4f-4e2b-9c61-0f3e2d4a5b6c",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dir.c_str(), dev, &info);
    if (NVSDK_NGX_FAILED(r)) { Log("NGX Init failed 0x%08X", r); return false; }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_ngxParams);
    if (NVSDK_NGX_FAILED(r)) { Log("NGX GetCapabilityParameters failed 0x%08X", r); return false; }
    int avail = 0;
    NVSDK_NGX_Parameter_GetI(g_ngxParams, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (!avail) { Log("DLSS not available (check the NVIDIA driver)"); return false; }
    g_ngxInit = true;
    Log("NGX initialized, DLSS available");
    return true;
}

static void ReleaseSized()
{
    SafeRelease(g_mvUAV); SafeRelease(g_mvTex); SafeRelease(g_depUAV); SafeRelease(g_depTex);
    SafeRelease(g_outTex); SafeRelease(g_colCopy);
    SafeRelease(g_outSRV); SafeRelease(g_shUAV); SafeRelease(g_shTex);
    if (g_dlss) { NVSDK_NGX_D3D11_ReleaseFeature(g_dlss); g_dlss = nullptr; }
}

static bool EnsureSized(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h, DXGI_FORMAT outFmt, DXGI_FORMAT colFmt)
{
    if (g_dlss && w == g_w && h == g_h && outFmt == g_outFmt && colFmt == g_colFmt) return true;
    ReleaseSized();
    g_w = w; g_h = h; g_outFmt = outFmt; g_colFmt = colFmt;
    const UINT uavsrv = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    g_mvTex  = MakeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, uavsrv);
    g_depTex = MakeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, uavsrv);
    g_outTex = MakeTex(dev, w, h, TypedOf(outFmt), uavsrv);
    g_shTex  = MakeTex(dev, w, h, TypedOf(outFmt), uavsrv);
    if (g_outTex && g_shTex) {
        if (FAILED(dev->CreateShaderResourceView(g_outTex, nullptr, &g_outSRV)) ||
            FAILED(dev->CreateUnorderedAccessView(g_shTex, nullptr, &g_shUAV))) Log("Sharpening view creation failed (sharpening unavailable)");
    }
    if (!g_mvTex || !g_depTex || !g_outTex) return false;
    if (TypedOf(colFmt) != colFmt) { g_colCopy = MakeTex(dev, w, h, TypedOf(colFmt), D3D11_BIND_SHADER_RESOURCE); if (!g_colCopy) return false; }
    if (FAILED(dev->CreateUnorderedAccessView(g_mvTex, nullptr, &g_mvUAV)) ||
        FAILED(dev->CreateUnorderedAccessView(g_depTex, nullptr, &g_depUAV))) { Log("UAV creation failed"); return false; }

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w; cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = w; cp.Feature.InTargetHeight = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              (g_cfg.diagMVJittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0);
    NVSDK_NGX_Parameter_SetUI(g_ngxParams, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned)g_cfg.preset);
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_dlss, g_ngxParams, &cp);
    if (NVSDK_NGX_FAILED(r)) { Log("DLAA feature creation failed 0x%08X", r); g_dlss = nullptr; return false; }
    Log("DLAA created: %ux%u, output fmt %d, input fmt %d, preset %d", w, h, (int)outFmt, (int)colFmt, g_cfg.preset);
    g_resetNext = true;
    return true;
}

static bool EnsureCBBuffers(ID3D11Device* dev, UINT size)
{
    D3D11_BUFFER_DESC cur = {};
    if (g_cbCopy) g_cbCopy->GetDesc(&cur);
    if (g_cbCopy && cur.ByteWidth == size) return true;
    SafeRelease(g_cbCopy);
    for (auto& s : g_cbStage) SafeRelease(s);
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &g_cbCopy))) { Log("Constant buffer copy creation failed"); return false; }
    D3D11_BUFFER_DESC s = {};
    s.ByteWidth = size; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& st : g_cbStage) if (FAILED(dev->CreateBuffer(&s, nullptr, &st))) { Log("Staging buffer creation failed"); return false; }
    for (auto& u : g_cbStageUsed) u = false;
    Log("TAA constant buffer size %u bytes", size);
    return true;
}

static void RepatchContext(ID3D11DeviceContext* ctx, const char* why);

// SwapDeviceContextState 는 후킹을 지우므로 사용하지 않고, 필요한 상태만 직접 저장/복원
struct SavedState {
    ID3D11ComputeShader* cs = nullptr; ID3D11ClassInstance* csCI[256] = {}; UINT csNCI = 256;
    ID3D11Buffer* csCB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
    ID3D11ShaderResourceView* csSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* csSmp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11UnorderedAccessView* csUAV[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* rs = nullptr; ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT sm = 0;
    ID3D11DepthStencilState* dss = nullptr; UINT sref = 0;
    ID3D11VertexShader* vs = nullptr; ID3D11ClassInstance* vsCI[256] = {}; UINT vsNCI = 256;
    ID3D11PixelShader* ps = nullptr; ID3D11ClassInstance* psCI[256] = {}; UINT psNCI = 256;
    ID3D11ShaderResourceView* psSRV[16] = {}; ID3D11SamplerState* psSmp[16] = {}; ID3D11Buffer* psCB[14] = {}; ID3D11Buffer* vsCB[14] = {};
    ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOff = 0; ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN; UINT ibOff = 0;

    void Save(ID3D11DeviceContext* c) {
        c->CSGetShader(&cs, csCI, &csNCI);
        c->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, csCB);
        c->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, csSRV);
        c->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, csSmp);
        c->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, csUAV);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->RSGetViewports(&nvp, vp); c->RSGetScissorRects(&nsc, sc); c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &sm); c->OMGetDepthStencilState(&dss, &sref);
        c->VSGetShader(&vs, vsCI, &vsNCI); c->PSGetShader(&ps, psCI, &psNCI);
        c->PSGetShaderResources(0, 16, psSRV); c->PSGetSamplers(0, 16, psSmp);
        c->PSGetConstantBuffers(0, 14, psCB); c->VSGetConstantBuffers(0, 14, vsCB);
        c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOff); c->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->CSSetShader(cs, csCI, csNCI);
        c->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, csCB);
        c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, csSRV);
        c->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, csSmp);
        c->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, csUAV, nullptr);
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->RSSetViewports(nvp, vp); c->RSSetScissorRects(nsc, sc); c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, sm); c->OMSetDepthStencilState(dss, sref);
        c->VSSetShader(vs, vsCI, vsNCI); c->PSSetShader(ps, psCI, psNCI);
        c->PSSetShaderResources(0, 16, psSRV); c->PSSetSamplers(0, 16, psSmp);
        c->PSSetConstantBuffers(0, 14, psCB); c->VSSetConstantBuffers(0, 14, vsCB);
        c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOff); c->IASetIndexBuffer(ib, ibFmt, ibOff);
    }
    ~SavedState() {
        SafeRelease(cs); for (UINT i = 0; i < csNCI && i < 256; ++i) SafeRelease(csCI[i]);
        for (auto& x : csCB) SafeRelease(x); for (auto& x : csSRV) SafeRelease(x);
        for (auto& x : csSmp) SafeRelease(x); for (auto& x : csUAV) SafeRelease(x);
        for (auto& x : rtv) SafeRelease(x); SafeRelease(dsv);
        SafeRelease(rs); SafeRelease(bs); SafeRelease(dss);
        SafeRelease(vs); for (UINT i = 0; i < vsNCI && i < 256; ++i) SafeRelease(vsCI[i]);
        SafeRelease(ps); for (UINT i = 0; i < psNCI && i < 256; ++i) SafeRelease(psCI[i]);
        for (auto& x : psSRV) SafeRelease(x); for (auto& x : psSmp) SafeRelease(x);
        for (auto& x : psCB) SafeRelease(x); for (auto& x : vsCB) SafeRelease(x);
        SafeRelease(il); SafeRelease(vb); SafeRelease(ib);
    }
};

// ================================================================== DLAA 실행 (TAA 완료 직후)
static bool g_loggedFirstRun = false;

static void RunDLAA(ID3D11DeviceContext* ctx, ID3D11Texture2D* outTex)
{
    if (g_fatal || !g_colorTex || !g_velSRV || !g_depSRV || !g_taaCB) {
        static bool warned = false;
        if (!warned) { warned = true; Log("DLAA cannot run: color=%p vel=%p dep=%p cb=%p fatal=%d", (void*)g_colorTex, (void*)g_velSRV, (void*)g_depSRV, (void*)g_taaCB, g_fatal ? 1 : 0); }
        return;
    }
    D3D11_TEXTURE2D_DESC cd, od; g_colorTex->GetDesc(&cd); outTex->GetDesc(&od);
    if (cd.Width != od.Width || cd.Height != od.Height || (int)cd.Width < g_cfg.minWidth) return;
    UINT w = cd.Width, h = cd.Height;

    if (!g_dev) ctx->GetDevice(&g_dev);
    if (!g_cs && !CreateCS(g_dev))                 { g_fatal = true; return; }
    if (!InitNGX(g_dev))                           { g_fatal = true; return; }
    if (g_presetDirty) { ReleaseSized(); g_presetDirty = false; }
    if (!EnsureSized(g_dev, ctx, w, h, od.Format, cd.Format)) { g_fatal = true; return; }
    D3D11_BUFFER_DESC bd; g_taaCB->GetDesc(&bd);
    if (bd.ByteWidth < 128 || !EnsureCBBuffers(g_dev, bd.ByteWidth)) { Log("Unexpected TAA constant buffer size (%u)", bd.ByteWidth); g_fatal = true; return; }

    SavedState* saved = new SavedState();
    saved->Save(ctx);
    // 렌더타깃 연결을 풀어 읽기/쓰기 충돌로 SRV 가 무시되지 않게 함
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    { ID3D11ShaderResourceView* nul[16] = {}; ctx->PSSetShaderResources(0, 16, nul); }

    // 2프레임 이상 지난 상수버퍼 사본 읽기 (대기 없이)
    for (int i = 0; i < 4; ++i) {
        if (!g_cbStageUsed[i] || g_cbStageFrame[i] + 2 > g_taaFrame) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_cbStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            float tmp[32]; memcpy(tmp, m.pData, 128);
            ctx->Unmap(g_cbStage[i], 0);
            g_cbStageUsed[i] = false;
            ObserveJitter(g_cbStageFrame[i], tmp, w, h);
        }
    }

    // 이번 프레임 상수버퍼: GPU 복사 (CS 용 + 나중에 읽을 스테이징)
    ctx->CopyResource(g_cbCopy, g_taaCB);
    int si = (int)(g_taaFrame & 3);
    ctx->CopyResource(g_cbStage[si], g_taaCB);
    g_cbStageFrame[si] = g_taaFrame; g_cbStageUsed[si] = true;

    // 지터 예측
    float jx = 0, jy = 0;
    bool haveJ = PredictJitter(g_taaFrame, &jx, &jy);
    g_jPredX[g_taaFrame & 7] = jx; g_jPredY[g_taaFrame & 7] = jy;
    float ex = jx, ey = jy;
    if (g_cfg.diagJitterSign & 1) ex = -ex;   // 진단용
    if (g_cfg.diagJitterSign & 2) ey = -ey;

    // 1) 모션벡터 + 깊이
    ID3D11ShaderResourceView* csSrv[2] = { g_velSRV, g_depSRV };
    ID3D11UnorderedAccessView* csUav[2] = { g_mvUAV, g_depUAV };
    ctx->CSSetShader(g_cs, nullptr, 0);
    if (!g_modeCB) {
        D3D11_BUFFER_DESC md = {}; md.ByteWidth = 16; md.Usage = D3D11_USAGE_DEFAULT; md.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&md, nullptr, &g_modeCB);
    }
    UINT modeData[4] = { (UINT)g_cfg.diagVelMode, 0, 0, 0 };
    if (g_modeCB) ctx->UpdateSubresource(g_modeCB, 0, nullptr, modeData, 0, 0);
    ID3D11Buffer* csCBs[2] = { g_cbCopy, g_modeCB };
    ctx->CSSetConstantBuffers(0, 2, csCBs);
    ctx->CSSetShaderResources(0, 2, csSrv);
    ctx->CSSetUnorderedAccessViews(0, 2, csUav, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* nS[2] = {}; ID3D11UnorderedAccessView* nU[2] = {};
    ctx->CSSetShaderResources(0, 2, nS);
    ctx->CSSetUnorderedAccessViews(0, 2, nU, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    // 2) 색상
    ID3D11Resource* color = g_colorTex;
    if (g_colCopy) { ctx->CopyResource(g_colCopy, g_colorTex); color = g_colCopy; }

    // 3) DLAA
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = color;
    ep.Feature.pInOutput = g_outTex;
    ep.pInDepth = g_depTex;
    ep.pInMotionVectors = g_mvTex;
    ep.InJitterOffsetX = ex;
    ep.InJitterOffsetY = ey;
    ep.InRenderSubrectDimensions.Width = w;
    ep.InRenderSubrectDimensions.Height = h;
    ep.InReset = g_resetNext ? 1 : 0;
    ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_dlss, g_ngxParams, &ep);
    bool ok = !NVSDK_NGX_FAILED(r);
    if (ok) {
        static bool casTried = false;
        if (g_cfg.sharpness > 0.001f && !g_casCS && !casTried) { casTried = true; CreateCAS(g_dev); }
        if (g_cfg.sharpness > 0.001f && g_casCS && g_outSRV && g_shUAV) {
            float sc[4] = { g_cfg.sharpness, 0, 0, 0 };
            ctx->UpdateSubresource(g_casCB, 0, nullptr, sc, 0, 0);
            ctx->CSSetShader(g_casCS, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_casCB);
            ctx->CSSetShaderResources(0, 1, &g_outSRV);
            ctx->CSSetUnorderedAccessViews(0, 1, &g_shUAV, nullptr);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ID3D11ShaderResourceView* n1 = nullptr; ID3D11UnorderedAccessView* u1 = nullptr;
            ctx->CSSetShaderResources(0, 1, &n1);
            ctx->CSSetUnorderedAccessViews(0, 1, &u1, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);
            ctx->CopyResource(outTex, g_shTex);
        } else {
            ctx->CopyResource(outTex, g_outTex);
        }
    }
    else Log("DLAA evaluation failed 0x%08X", r);

    saved->Restore(ctx);
    delete saved;
    RepatchContext(ctx, "after DLAA");

    if (!ok) { g_fatal = true; return; }
    g_resetNext = false;
    g_lastDLAAGameFrame = g_gameFrame;
    if (!g_loggedFirstRun) { g_loggedFirstRun = true; Log("DLAA first run OK (%ux%u)", w, h); }
    if (g_frame < 20 || g_cfg.verbose)
        Log("DLAA #%llu (TAA frame %llu): predicted jitter (%.4f, %.4f) %s", (unsigned long long)g_frame, (unsigned long long)g_taaFrame, jx, jy, haveJ ? "" : "(not yet)");
    ++g_frame;
}

// ================================================================== 컨텍스트 훅
static void STDMETHODCALLTYPE hkPSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
{
    HookedCtx* v = FindCtx(ctx);
    ++g_statPS;
    g_curPS = ps;
    v->PSSetShader(ctx, ps, ci, n);
}

static void STDMETHODCALLTYPE hkPSSetConstantBuffers(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11Buffer* const* bufs)
{
    HookedCtx* v = FindCtx(ctx);
    if (!g_inDLAA && g_taaPS && g_curPS == g_taaPS && start == 0 && n >= 1 && bufs && bufs[0] && bufs[0] != g_taaCB)
        Assign(g_taaCB, bufs[0]);
    v->PSSetCB(ctx, start, n, bufs);
}

static int g_nearMiss = 0;

static void STDMETHODCALLTYPE hkPSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
{
    HookedCtx* v = FindCtx(ctx);
    ++g_statSRV;
    if (!g_inDLAA && views && start == 0 && n >= 1) {
        // (1) TAA 입력 연결 감지: t1=1010102, t2=RG16, t3=depth
        if (n >= 4 && views[0] && views[1] && views[2] && views[3] && IsRG16(SrvFmt(views[2]))) {
            if (Is1010102(SrvFmt(views[1])) && IsDepthSRV(SrvFmt(views[3]))) {
                ++g_statTAABind;
                ++g_taaFrame;   // 게임 지터는 TAA 가 한 번 돌 때마다 한 칸 진행
                ID3D11Texture2D* col = TexOf(views[0]);
                ID3D11Texture2D* hist = TexOf(views[1]);
                SafeRelease(g_colorTex); g_colorTex = col;
                Assign(g_velSRV, views[2]);
                Assign(g_depSRV, views[3]);
                SafeRelease(g_curHist); g_curHist = hist;
                if (hist && hist != g_histA && hist != g_histB) {
                    if (!g_histA) Assign(g_histA, hist);
                    else if (!g_histB) Assign(g_histB, hist);
                    else { Assign(g_histA, g_histB); Assign(g_histB, hist); }
                    Log("TAA history texture registered %p (A=%p B=%p)", (void*)hist, (void*)g_histA, (void*)g_histB);
                }
                if (g_taaPS != g_curPS) { g_taaPS = g_curPS; Log("TAA input binding detected (PS %p)", (void*)g_curPS); }
                ID3D11Buffer* cb = nullptr; ctx->PSGetConstantBuffers(0, 1, &cb);
                if (cb) { Assign(g_taaCB, cb); cb->Release(); }
                g_pendingTAA = true;
            } else if (g_nearMiss < 10) {
                ++g_nearMiss;
                Log("TAA candidate mismatch: t0=%d t1=%d t2=%d t3=%d", (int)SrvFmt(views[0]), (int)SrvFmt(views[1]), (int)SrvFmt(views[2]), (int)SrvFmt(views[3]));
            }
        }
        // (2) TAA 출력이 다음 패스 입력으로 연결됨 = TAA 완료
        else if (g_pendingTAA) {
            for (UINT i = 0; i < n && g_pendingTAA; ++i) {
                if (!views[i]) continue;
                ID3D11Texture2D* t = TexOf(views[i]);
                if (t && t != g_curHist && (t == g_histA || t == g_histB)) {
                    g_pendingTAA = false;
                    ++g_statTrigger;
                    if (g_cfg.enabled && !g_doneThisFrame) {
                        g_inDLAA = true;
                        RunDLAA(ctx, t);
                        g_inDLAA = false;
                        g_doneThisFrame = true;
                    }
                }
                SafeRelease(t);
            }
        }
    }
    v->PSSetSRV(ctx, start, n, views);
}

static void PatchContext(ID3D11DeviceContext* ctx)
{
    std::lock_guard<std::mutex> lk(g_vtLock);
    void** vt = *(void***)ctx;
    for (int i = 0; i < g_ctxVtCount; ++i) if (g_ctxVt[i].vt == vt) return;
    if (g_ctxVtCount >= 16 || vt[Vtbl_PSSetShader()] == (void*)hkPSSetShader) return;
    HookedCtx& h = g_ctxVt[g_ctxVtCount];
    h.vt = vt;
    h.PSSetShader = (PFN_PSSetShader)vt[Vtbl_PSSetShader()];
    h.PSSetSRV    = (PFN_PSSetSRV)vt[Vtbl_PSSetShaderResources()];
    h.PSSetCB     = (PFN_PSSetCB)vt[Vtbl_PSSetConstantBuffers()];
    MemoryBarrier(); ++g_ctxVtCount; MemoryBarrier();
    WriteSlot(vt, Vtbl_PSSetShader(),          (void*)hkPSSetShader);
    WriteSlot(vt, Vtbl_PSSetShaderResources(), (void*)hkPSSetShaderResources);
    WriteSlot(vt, Vtbl_PSSetConstantBuffers(), (void*)hkPSSetConstantBuffers);
    Log("Context vtable patched (%p)", (void*)vt);
}

// d3d11 이 슬롯을 다시 써서 후킹이 지워졌으면, 현재 값을 새 원본으로 저장하고 다시 후킹
static int g_repatchLogs = 0;
static void RepatchContext(ID3D11DeviceContext* ctx, const char* why)
{
    HookedCtx* h = FindCtx(ctx);
    if (!h) return;
    void** vt = h->vt;
    bool changed = false;
    void* cur;
    cur = vt[Vtbl_PSSetShader()];
    if (cur != (void*)hkPSSetShader) { h->PSSetShader = (PFN_PSSetShader)cur; WriteSlot(vt, Vtbl_PSSetShader(), (void*)hkPSSetShader); changed = true; }
    cur = vt[Vtbl_PSSetShaderResources()];
    if (cur != (void*)hkPSSetShaderResources) { h->PSSetSRV = (PFN_PSSetSRV)cur; WriteSlot(vt, Vtbl_PSSetShaderResources(), (void*)hkPSSetShaderResources); changed = true; }
    cur = vt[Vtbl_PSSetConstantBuffers()];
    if (cur != (void*)hkPSSetConstantBuffers) { h->PSSetCB = (PFN_PSSetCB)cur; WriteSlot(vt, Vtbl_PSSetConstantBuffers(), (void*)hkPSSetConstantBuffers); changed = true; }
    if (changed && g_repatchLogs < 5) { ++g_repatchLogs; Log("Hooks re-applied (%s)", why); }
}


// ================================================================== 오버레이 UI (Dear ImGui, ReShade 스타일)
static bool      g_menuOpen = false;
static ULONGLONG g_toastUntil = 0;
static ULONGLONG g_bannerUntil = 0;
static std::string g_toast;
static int       g_captureKey = 0;   // 0 = 없음, 1 = 메뉴 키, 2 = DLAA 토글 키 입력 대기

// SDK 310.9: A-D removed, G/H/I/N/O revert to default, so only these are offered
static const int   PRESET_COUNT = 7;
static const int   kPresetVals[PRESET_COUNT]  = { 0, 5, 6, 10, 11, 12, 13 };
static const int   kDefaultPresetIdx = 4;      // K
static const char* kPresetShort[PRESET_COUNT] = { "Default", "E", "F", "J", "K", "L", "M" };
static const char* kPresetNamesEn[PRESET_COUNT] = { "Default (driver)", "E  (CNN, legacy)", "F  (CNN, legacy)", "J  (transformer)", "K  (transformer, recommended)", "L  (transformer)", "M  (transformer)" };
static const char* kPresetNamesKo[PRESET_COUNT] = { u8"기본 (드라이버)", u8"E  (CNN, 구형)", u8"F  (CNN, 구형)", u8"J  (트랜스포머)", u8"K  (트랜스포머, 권장)", u8"L  (트랜스포머)", u8"M  (트랜스포머)" };
static const char* kPresetDescEn[PRESET_COUNT]  = {
    "Lets the DLSS DLL / driver choose. With current DLLs this is normally K, but it may change with driver updates.",
    "Older CNN model, marked deprecated by NVIDIA. Lighter, but softer with more ghosting. May not be available in future DLLs.",
    "Older CNN model, marked deprecated by NVIDIA. Originally the DLAA / Ultra Performance preset. May not be available in future DLLs.",
    "Transformer model similar to K. Slightly less ghosting, but more flickering. K is generally recommended over J.",
    "Transformer model. NVIDIA's default preset for DLAA, with the best image quality and the highest cost.",
    "Transformer model. Originally the default for Ultra Performance mode; sharper. This game's short jitter cycle (8 frames) can make object edges shimmer.",
    "Transformer model. Originally the default for Performance mode; sharper. This game's short jitter cycle (8 frames) can make object edges shimmer." };
static const char* kPresetDescKo[PRESET_COUNT]  = {
    u8"DLSS DLL/드라이버가 고릅니다. 지금 DLL에서는 보통 K 이지만 드라이버 업데이트에 따라 바뀔 수 있습니다.",
    u8"구형 CNN 모델로 NVIDIA가 지원 종료 예정으로 표시했습니다. 가볍지만 더 흐리고 잔상이 많습니다. 이후 DLL에서는 쓸 수 없을 수 있습니다.",
    u8"구형 CNN 모델로 NVIDIA가 지원 종료 예정으로 표시했습니다. 원래 DLAA/울트라 퍼포먼스용 프리셋입니다. 이후 DLL에서는 쓸 수 없을 수 있습니다.",
    u8"K 와 비슷한 트랜스포머 모델. 잔상은 조금 적지만 깜빡임이 더 있습니다. 보통 J 보다 K 를 권장합니다.",
    u8"트랜스포머 모델. NVIDIA가 DLAA에 기본으로 쓰는 프리셋으로 화질이 가장 좋고 연산량도 가장 많습니다.",
    u8"트랜스포머 모델. 원래 울트라 퍼포먼스 모드용 기본 프리셋으로 더 선명합니다. 이 게임은 지터 주기가 짧아(8프레임) 물체 가장자리가 자글거릴 수 있습니다.",
    u8"트랜스포머 모델. 원래 퍼포먼스 모드용 기본 프리셋으로 더 선명합니다. 이 게임은 지터 주기가 짧아(8프레임) 물체 가장자리가 자글거릴 수 있습니다." };
static const char* PresetName(int i) { return g_cfg.lang == 1 ? kPresetNamesKo[i] : kPresetNamesEn[i]; }
static const char* PresetDesc(int i) { return g_cfg.lang == 1 ? kPresetDescKo[i] : kPresetDescEn[i]; }

static int PresetIndex() { for (int i = 0; i < PRESET_COUNT; ++i) if (kPresetVals[i] == g_cfg.preset) return i; return kDefaultPresetIdx; }

static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string KeyName(int vk)
{
    switch (vk) {
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {   // 확장 키
    case VK_HOME: case VK_END: case VK_INSERT: case VK_DELETE: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= (1 << 24); break;
    }
    wchar_t n[64] = {};
    if (GetKeyNameTextW(lp, n, 64) > 0) return ToUtf8(n);
    char b[16]; sprintf_s(b, "0x%02X", vk); return b;
}

static void Toast(const std::string& t) { g_toast = t; g_toastUntil = GetTickCount64() + 2000; }

static void SaveConfig()
{
    const wchar_t* s = L"DLAA"; const wchar_t* f = g_ini.c_str();
    wchar_t b[16];
    WritePrivateProfileStringW(s, L"Enabled", g_cfg.enabled ? L"1" : L"0", f);
    swprintf_s(b, L"%d", g_cfg.preset);    WritePrivateProfileStringW(s, L"Preset", b, f);
    swprintf_s(b, L"%d", g_cfg.keyMenu);   WritePrivateProfileStringW(s, L"MenuKey", b, f);
    swprintf_s(b, L"%d", g_cfg.keyToggle); WritePrivateProfileStringW(s, L"ToggleKey", b, f);
    swprintf_s(b, L"%d", (int)lroundf(g_cfg.sharpness * 100)); WritePrivateProfileStringW(s, L"Sharpness", b, f);
    WritePrivateProfileStringW(s, L"NoCrashCams", g_cfg.noCrashCams ? L"1" : L"0", f);
    WritePrivateProfileStringW(s, L"Language", g_cfg.lang == 1 ? L"ko" : L"en", f);
#if NFS_DLAA_DIAG
    swprintf_s(b, L"%d", g_cfg.diagJitterSign); WritePrivateProfileStringW(s, L"DiagJitterSign", b, f);
    WritePrivateProfileStringW(s, L"DiagMVJittered", g_cfg.diagMVJittered ? L"1" : L"0", f);
    swprintf_s(b, L"%d", g_cfg.diagVelMode); WritePrivateProfileStringW(s, L"DiagVelMode", b, f);
#else
    // 진단 항목 정리 (이전 버전이 남긴 키 삭제)
    WritePrivateProfileStringW(s, L"DiagJitterSign", nullptr, f);
    WritePrivateProfileStringW(s, L"DiagMVJittered", nullptr, f);
    WritePrivateProfileStringW(s, L"DiagVelMode", nullptr, f);
#endif
    Log("Settings saved: Enabled=%d Preset=%d Sharpness=%d MenuKey=%d ToggleKey=%d Language=%s", g_cfg.enabled ? 1 : 0, g_cfg.preset, (int)lroundf(g_cfg.sharpness * 100), g_cfg.keyMenu, g_cfg.keyToggle, g_cfg.lang == 1 ? "ko" : "en");
}

static void SetEnabled(bool on)
{
    g_cfg.enabled = on; g_resetNext = true;
    Log("DLAA %s", on ? "on" : "off (game TAA)");
    Toast(on ? TR("DLAA on", u8"DLAA 켜짐") : TR("DLAA off (game TAA)", u8"DLAA 꺼짐 (게임 TAA)"));
    SaveConfig();
}
static void SetPresetIdx(int i)
{
    i = (i + PRESET_COUNT) % PRESET_COUNT;
    if (g_cfg.preset == kPresetVals[i]) return;
    g_cfg.preset = kPresetVals[i]; g_presetDirty = true;
    Log("Preset -> %d", g_cfg.preset);
    Toast(std::string(TR("DLSS preset ", u8"DLSS 프리셋 ")) + kPresetShort[i]);
    SaveConfig();
}

static bool GameFocused()
{
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyPressed(int vk)
{
    static bool prev[256] = {};
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool r = d && !prev[vk & 255];
    prev[vk & 255] = d;
    return r;
}

// 단축키 처리 (Present 마다 호출)
static void HandleInput()
{
    if (g_captureKey) {
        // 키 지정 대기: 처음 눌린 키를 할당 (Esc = 취소, 마우스 버튼 제외)
        static bool prevAll[256] = {};
        static bool armed = false;
        int got = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;   // 좌/우 구분 키로 받음
            bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (armed && d && !prevAll[vk] && !got) got = vk;
            prevAll[vk] = d;
        }
        armed = true;
        if (got && GameFocused()) {
            if (got != VK_ESCAPE) {
                if (g_captureKey == 1) g_cfg.keyMenu = got; else g_cfg.keyToggle = got;
                SaveConfig();
                Toast(std::string(TR("Hotkey set: ", u8"단축키 지정: ")) + KeyName(got));
            }
            g_captureKey = 0; armed = false;
            KeyPressed(g_cfg.keyMenu); KeyPressed(g_cfg.keyToggle);   // 방금 누른 키로 바로 동작하지 않게
        }
        return;
    }
    bool kMenu = KeyPressed(g_cfg.keyMenu), kTog = KeyPressed(g_cfg.keyToggle);
    if (!GameFocused()) return;
    if (kMenu) g_menuOpen = !g_menuOpen;
    if (kTog) SetEnabled(!g_cfg.enabled);
}

// ---------------------------------------------------------------- ImGui 연결
static bool    g_uiInit = false, g_uiFailed = false;
static HWND    g_gameHwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static std::string g_uiIniPath;
static float   g_uiScale = 1.0f;

static LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_uiInit) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        if (g_menuOpen) {
            // 메뉴가 열려 있으면 마우스/키보드 입력을 게임에 넘기지 않음
            bool mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
            bool key = m >= WM_KEYFIRST && m <= WM_KEYLAST;
            if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
            if (mouse || key) return 0;
            if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

static void ApplyStyle(float s)
{
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0; st.ChildRounding = 0; st.FrameRounding = 0; st.PopupRounding = 0;
    st.ScrollbarRounding = 0; st.GrabRounding = 0; st.TabRounding = 0;
    st.WindowBorderSize = 1; st.FrameBorderSize = 0; st.TabBorderSize = 0;
    st.WindowPadding = ImVec2(10, 10); st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(8, 7);
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    ImVec4* c = st.Colors;
    const ImVec4 bg(0.11f, 0.11f, 0.12f, 0.96f), fr(0.20f, 0.20f, 0.22f, 1.00f), acc(0.26f, 0.52f, 0.86f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.55f, 0.57f, 1.00f);
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_PopupBg]              = ImVec4(0.13f, 0.13f, 0.14f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    c[ImGuiCol_FrameBg]              = fr;
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.27f, 0.27f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.32f, 0.32f, 0.36f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_CheckMark]            = acc;
    c[ImGuiCol_SliderGrab]           = acc;
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.36f, 0.62f, 0.95f, 1.00f);
    c[ImGuiCol_Button]               = fr;
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.26f, 0.52f, 0.86f, 0.60f);
    c[ImGuiCol_ButtonActive]         = acc;
    c[ImGuiCol_Header]               = ImVec4(0.26f, 0.52f, 0.86f, 0.45f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.26f, 0.52f, 0.86f, 0.65f);
    c[ImGuiCol_HeaderActive]         = acc;
    c[ImGuiCol_Separator]            = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.26f, 0.52f, 0.86f, 0.70f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.26f, 0.52f, 0.86f, 0.85f);
    c[ImGuiCol_TabSelectedOverline]  = acc;
    st.ScaleAllSizes(s);
}

static bool InitUI(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT bbHeight)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) { Log("UI: game window not found"); return false; }
    g_gameHwnd = d.OutputWindow;
    g_uiScale = std::max(1.0f, bbHeight / 1080.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g_uiIniPath = ToUtf8(g_dir) + "\\NFS_DLAA_ui.ini";   // 창 위치/크기 기억
    io.IniFilename = g_uiIniPath.c_str();
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    wchar_t win[MAX_PATH] = {}; GetWindowsDirectoryW(win, MAX_PATH);
    std::string font = ToUtf8(win) + "\\Fonts\\malgun.ttf";
    if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(font.c_str(), 18.0f * g_uiScale);
    else { Log("UI: Malgun Gothic not found, using default font (Korean text will not display)"); io.Fonts->AddFontDefault(); }

    ApplyStyle(g_uiScale);
    if (!ImGui_ImplWin32_Init(g_gameHwnd) || !ImGui_ImplDX11_Init(dev, ctx)) { Log("UI: ImGui backend init failed"); return false; }
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
    if (!g_origWndProc) { Log("UI: window message hook failed (no mouse input)"); }
    Log("UI ready (ImGui %s, scale %.2f)", IMGUI_VERSION, g_uiScale);
    return true;
}

static ID3D11Device* g_uiDev = nullptr;   // device the UI was created on (compare only)

// The game can create a short-lived start-up window/device before the real one (seen when the ASI loader
// loads us early). If the UI was built on that one, tear it down so it can be rebuilt on the real window.
static void ShutdownUI(const char* why)
{
    if (!g_uiInit) return;
    Log("UI: rebuilding (%s)", why);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_origWndProc && g_gameHwnd && IsWindow(g_gameHwnd) &&
        (WNDPROC)GetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC) == HookWndProc)
        SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    g_origWndProc = nullptr;
    g_gameHwnd = nullptr;
    g_uiDev = nullptr;
    g_uiInit = false;
}

// ---------------------------------------------------------------- 화면 구성
static void HelpMarker(const char* t)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void StatusLine()
{
    ImVec4 col; const char* txt;
    if (g_fatal)                                                   { col = ImVec4(1.0f, 0.42f, 0.42f, 1); txt = TR("Error - check the Stats tab and NFS_DLAA.log", u8"오류 - 통계 탭과 NFS_DLAA.log 를 확인하세요"); }
    else if (!g_cfg.enabled)                                       { col = ImVec4(0.6f, 0.6f, 0.62f, 1);  txt = TR("Off - using the game's TAA", u8"꺼짐 - 게임 기본 TAA 사용 중"); }
    else if (g_frame > 0 && g_lastDLAAGameFrame + 5 >= g_gameFrame) { col = ImVec4(0.45f, 0.90f, 0.45f, 1); txt = TR("DLAA active", u8"DLAA 동작 중"); }
    else                                                           { col = ImVec4(0.95f, 0.80f, 0.30f, 1); txt = TR("Waiting - runs during gameplay", u8"대기 중 - 주행 화면에서 동작합니다"); }
    ImGui::TextColored(col, u8"●"); ImGui::SameLine(); ImGui::TextColored(col, "%s", txt);
}

static bool KeyButton(const char* label, int which, int vk)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
    std::string btn = (g_captureKey == which) ? std::string(TR("Press a key... (Esc to cancel)", u8"키를 누르세요... (Esc 취소)")) : KeyName(vk);
    btn += "##key" + std::to_string(which);
    bool clicked = ImGui::Button(btn.c_str(), ImVec2(ImGui::GetFontSize() * 13.0f, 0));
    if (clicked) g_captureKey = which;
    return clicked;
}

static void DrawMenu()
{
    const float f = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(40 * g_uiScale, 40 * g_uiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(f * 30, f * 22), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(u8"NFS Hot Pursuit Remastered  ·  DLAA###NFS_DLAA", &g_menuOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("##tabs")) {
        // ---- 홈
        if (ImGui::BeginTabItem(TR("Home###home", u8"홈###home"))) {
            StatusLine();
            ImGui::Separator();

            bool en = g_cfg.enabled;
            if (ImGui::Checkbox(TR("Enable DLAA", u8"DLAA 사용"), &en)) SetEnabled(en);
            HelpMarker(TR("When off, the game's built-in TAA is used. Toggle it to compare image quality.", u8"끄면 게임에 내장된 TAA로 그립니다. 켜고 끄며 화질을 비교해보세요."));

            int idx = PresetIndex();
            ImGui::SetNextItemWidth(f * 16);
            if (ImGui::BeginCombo(TR("DLSS preset###preset", u8"DLSS 프리셋###preset"), PresetName(idx))) {
                for (int i = 0; i < PRESET_COUNT; ++i) {
                    bool sel = (i == idx);
                    if (ImGui::Selectable(PresetName(i), sel)) SetPresetIdx(i);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PresetDesc(i));
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", PresetDesc(PresetIndex()));
            ImGui::PopTextWrapPos();

            {
                int sp = (int)lroundf(g_cfg.sharpness * 100);
                ImGui::SetNextItemWidth(f * 16);
                if (ImGui::SliderInt(TR("Sharpening###sharp", u8"선명도###sharp"), &sp, 0, 100, sp == 0 ? TR("Off", u8"끄기") : "%d%%")) g_cfg.sharpness = sp / 100.0f;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    SaveConfig();
                    Toast(sp ? std::string(TR("Sharpening ", u8"선명도 ")) + std::to_string(sp) + "%" : std::string(TR("Sharpening off", u8"선명도 끄기")));
                }
                HelpMarker(TR("Adds adaptive sharpening (AMD CAS style) after DLAA. Soft areas get sharper while edges that are already sharp are left mostly alone. "
                              "NVIDIA removed DLSS's own sharpening, so this mod does it itself. With preset K, around 30-60% gives sharpness close to L/M without the shimmering.",
                              u8"DLAA 결과에 적응형 선명화(AMD CAS 방식)를 더합니다. 흐린 부분은 더 선명하게, 이미 선명한 가장자리는 덜 건드립니다. "
                              u8"NVIDIA가 DLSS 자체 선명화 기능을 없앴기 때문에 모드가 직접 처리합니다. 프리셋 K 와 함께 30~60% 정도면 자글거림 없이 L/M 에 가까운 선명함을 얻을 수 있습니다."));
            }

            ImGui::Spacing(); ImGui::Separator();
            {
                bool cc = g_cfg.noCrashCams;
                if (ImGui::Checkbox(TR("Disable crash cams", u8"충돌 카메라 끄기"), &cc)) {
                    g_cfg.noCrashCams = cc; SaveConfig();
                    Toast(cc ? TR("Crash cams off", u8"충돌 카메라 꺼짐") : TR("Crash cams on", u8"충돌 카메라 켜짐"));
                }
                HelpMarker(TR("Skips the slow-motion wreck camera when you crash, so you keep driving right away. "
                              "Takedown and spike strip cameras are not affected. Don't use this together with ZPatch's NoCrashCams option.",
                              u8"사고가 났을 때 나오는 슬로 모션 충돌 카메라를 건너뛰고 바로 주행을 이어갑니다. "
                              u8"테이크다운과 스파이크 스트립 카메라에는 영향이 없습니다. ZPatch 의 NoCrashCams 옵션과 같이 쓰지 마세요."));
                if (g_ccState == 3)
                    ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1), "%s", TR("Not available for this game version (see NFS_DLAA.log)", u8"이 게임 버전에서는 사용할 수 없습니다 (NFS_DLAA.log 확인)"));
            }

            ImGui::Spacing(); ImGui::Separator();
            ImGui::TextDisabled(TR("%s: open/close menu    %s: toggle DLAA", u8"%s: 메뉴 열기/닫기    %s: DLAA 켜기/끄기"), KeyName(g_cfg.keyMenu).c_str(), KeyName(g_cfg.keyToggle).c_str());
            ImGui::TextDisabled("%s", TR("While the menu is open, mouse and keyboard input is not sent to the game.", u8"메뉴가 열려 있는 동안 마우스·키보드 입력은 게임에 전달되지 않습니다."));
            ImGui::EndTabItem();
        }
        // ---- 통계
        if (ImGui::BeginTabItem(TR("Stats###stats", u8"통계###stats"))) {
            if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn(TR("Item", u8"항목"), ImGuiTableColumnFlags_WidthFixed, f * 12);
                ImGui::TableSetupColumn(TR("Value", u8"값"));
                auto row = [](const char* k, const char* v) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(k); ImGui::TableNextColumn(); ImGui::TextUnformatted(v); };
                char b[96];
                if (g_w) sprintf_s(b, "%u x %u", g_w, g_h); else sprintf_s(b, "%s", TR("None yet", u8"아직 없음"));
                row(TR("DLAA resolution", u8"DLAA 해상도"), b);
                row(TR("DLSS preset", u8"DLSS 프리셋"), kPresetShort[PresetIndex()]);
                if (g_cfg.sharpness > 0.001f) sprintf_s(b, "%d%%", (int)lroundf(g_cfg.sharpness * 100)); else sprintf_s(b, "%s", TR("Off", u8"끄기"));
                row(TR("Sharpening", u8"선명도"), b);
                row(TR("Crash cams", u8"충돌 카메라"),
                    g_ccState == 3 ? TR("Not available", u8"사용 불가") :
                    g_ccApplied ? TR("Disabled", u8"꺼짐") : TR("Game default", u8"게임 기본"));
                sprintf_s(b, "%llu", (unsigned long long)g_frame);                 row(TR("DLAA frames", u8"DLAA 처리 프레임"), b);
                sprintf_s(b, "%ld", g_statTAABind);                                 row(TR("TAA passes found", u8"TAA 패스 감지"), b);
                sprintf_s(b, "%ld", g_statMissed);                                  row(TR("TAA passes missed", u8"TAA 완료 놓침"), b);
                if (g_jChecks) sprintf_s(b, "%d%%  (%d / %d)", g_jHits * 100 / g_jChecks, g_jHits, g_jChecks); else sprintf_s(b, "-");
                row(TR("Jitter prediction", u8"지터 예측 정확도"), b);
                if (g_jPeriod) sprintf_s(b, TR("%d frames", u8"%d 프레임"), g_jPeriod); else sprintf_s(b, "-");
                row(TR("Jitter cycle", u8"지터 주기"), b);
                row(TR("Errors", u8"오류"), g_fatal ? TR("Yes (see log)", u8"있음 (로그 확인)") : TR("None", u8"없음"));
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        // ---- 설정
        if (ImGui::BeginTabItem(TR("Settings###settings", u8"설정###settings"))) {
            KeyButton(TR("Open/close menu", u8"메뉴 열기/닫기"), 1, g_cfg.keyMenu);
            KeyButton(TR("Toggle DLAA", u8"DLAA 켜기/끄기"), 2, g_cfg.keyToggle);
            {
                static const char* langs[2] = { "English", u8"한국어" };
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(u8"Language / 언어");
                ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
                int lg = g_cfg.lang;
                if (ImGui::Combo("##lang", &lg, langs, 2) && lg != g_cfg.lang) {
                    g_cfg.lang = lg; SaveConfig();
                    Log("Language -> %s", lg == 1 ? "ko" : "en");
                }
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            if (ImGui::Button(TR("Reset to defaults", u8"기본값으로 되돌리기"))) {
                g_cfg.keyMenu = VK_F8; g_cfg.keyToggle = VK_F9;
                if (!g_cfg.enabled) SetEnabled(true);
                SetPresetIdx(kDefaultPresetIdx);
                g_cfg.sharpness = 0.0f;
                g_cfg.noCrashCams = false;
                SaveConfig();
                Toast(TR("Settings reset to defaults", u8"기본값으로 되돌렸습니다"));
            }
            ImGui::TextDisabled("%s", TR("All changes are saved to NFS_DLAA.ini immediately.", u8"모든 변경 사항은 NFS_DLAA.ini 에 바로 저장됩니다."));

#if NFS_DLAA_DIAG
            ImGui::Spacing();
            if (ImGui::CollapsingHeader(u8"진단 (임시)", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled(u8"프리셋 M 으로 두고 차를 세운 상태에서 바꿔보세요. 가장자리가 가장 조용한 조합을 찾으면 됩니다.");
                ImGui::PopTextWrapPos();
                static const char* signs[4] = { u8"0: 그대로", u8"1: X 반전", u8"2: Y 반전", u8"3: X·Y 반전" };
                ImGui::SetNextItemWidth(f * 12);
                int js = g_cfg.diagJitterSign;
                if (ImGui::Combo(u8"지터 부호", &js, signs, 4)) {
                    g_cfg.diagJitterSign = js; g_resetNext = true; SaveConfig();
                    Toast(std::string(u8"지터 부호 ") + signs[js]);
                    Log("Diag: jitter sign -> %d", js);
                }
                bool mj = g_cfg.diagMVJittered;
                if (ImGui::Checkbox(u8"모션 벡터에 지터 포함 (MVJittered)", &mj)) {
                    g_cfg.diagMVJittered = mj; g_presetDirty = true; SaveConfig();
                    Toast(mj ? u8"MVJittered 켜짐" : u8"MVJittered 꺼짐");
                    Log("Diag: MVJittered -> %d", mj ? 1 : 0);
                }
                static const char* vm[4] = { u8"0: 그대로", u8"1: 지터 차이 빼기", u8"2: 지터 차이 더하기", u8"3: 속도 버퍼 무시" };
                ImGui::SetNextItemWidth(f * 12);
                int vmode = g_cfg.diagVelMode;
                if (ImGui::Combo(u8"속도 버퍼 처리", &vmode, vm, 4)) {
                    g_cfg.diagVelMode = vmode; g_resetNext = true; SaveConfig();
                    Toast(std::string(u8"속도 버퍼 ") + vm[vmode]);
                    Log("Diag: velocity buffer mode -> %d", vmode);
                }
                HelpMarker(u8"3번은 움직이는 차의 모션 벡터를 버리므로 주행 중에는 쓰면 안 됩니다. 정지 화면 비교용입니다.");
                ImGui::TextDisabled(u8"현재 조합: 부호 %d / MVJittered %s / 속도 버퍼 %d", g_cfg.diagJitterSign, g_cfg.diagMVJittered ? u8"켜짐" : u8"꺼짐", g_cfg.diagVelMode);
            }
#endif
            ImGui::EndTabItem();
        }
        // ---- 정보
        if (ImGui::BeginTabItem(TR("About###about", u8"정보###about"))) {
            ImGui::TextUnformatted(TR("NFS Hot Pursuit Remastered DLAA mod", u8"NFS Hot Pursuit Remastered DLAA 모드"));
            ImGui::TextDisabled("%s", TR("Replaces the game's TAA pass with NVIDIA DLSS (DLAA mode).", u8"게임 TAA 패스를 NVIDIA DLSS(DLAA 모드)로 대체합니다."));
            ImGui::Spacing();
            ImGui::BulletText(u8"NVIDIA DLSS SDK 310.9");
            ImGui::BulletText(TR("Dear ImGui %s (MIT license)", u8"Dear ImGui %s (MIT 라이선스)"), IMGUI_VERSION);
            ImGui::Spacing();
            ImGui::TextDisabled("%s", TR("Log: NFS_DLAA.log in the game folder", u8"로그: 게임 폴더의 NFS_DLAA.log"));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void DrawToastAndBanner(bool toast, bool banner)
{
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (banner) {
        // ReShade 처럼 시작 시 화면 위쪽에 안내
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 10 * g_uiScale), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.80f);
        if (ImGui::Begin("##banner", nullptr, fl)) {
            ImGui::TextUnformatted(TR("NFS DLAA mod loaded", u8"NFS DLAA 모드 로드됨"));
            ImGui::TextDisabled(TR("Press %s to open the settings menu  ·  DLSS preset %s", u8"%s 키로 설정 메뉴를 엽니다  ·  DLSS 프리셋 %s"), KeyName(g_cfg.keyMenu).c_str(), kPresetShort[PresetIndex()]);
        }
        ImGui::End();
    }
    if (toast) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 20 * g_uiScale, vp->Pos.y + 20 * g_uiScale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.80f);
        if (ImGui::Begin("##toast", nullptr, fl)) ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
}

static void DrawOverlay(IDXGISwapChain* sc)
{
    ULONGLONG now = GetTickCount64();
    bool toast = now < g_toastUntil && !g_menuOpen;
    bool banner = now < g_bannerUntil && !g_menuOpen;
    if ((!g_menuOpen && !toast && !banner) || g_uiFailed) return;

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    ID3D11Texture2D* bb = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    g_inDLAA = true;
    do {
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) break;
        D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
        DXGI_SWAP_CHAIN_DESC scd = {};
        if (FAILED(sc->GetDesc(&scd))) break;
        if (g_uiInit && (dev != g_uiDev || scd.OutputWindow != g_gameHwnd || !IsWindow(g_gameHwnd)))
            ShutdownUI(dev != g_uiDev ? "new device" : "new window");
        if (!g_uiInit) {
            // build the UI only on the real game window, not on a tiny or hidden start-up one
            if (bd.Width < 640 || bd.Height < 480 || !scd.OutputWindow || !IsWindowVisible(scd.OutputWindow)) break;
            if (!InitUI(sc, dev, ctx, bd.Height)) { g_uiFailed = true; break; }
            g_uiInit = true;
            g_uiDev = dev;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = TypedOf(bd.Format);
        rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(bb, &rd, &rtv))) {
            static bool w = false; if (!w) { w = true; Log("UI: backbuffer RTV creation failed (fmt %d)", (int)bd.Format); }
            break;
        }

        SavedState* saved = new SavedState();
        saved->Save(ctx);
        ID3D11GeometryShader* gs = nullptr; ID3D11ClassInstance* gsCI[256] = {}; UINT gsN = 256;
        ctx->GSGetShader(&gs, gsCI, &gsN);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::GetIO().MouseDrawCursor = g_menuOpen;   // 게임이 커서를 숨겨도 보이도록 ImGui 가 직접 그림
        ImGui::NewFrame();
        DrawToastAndBanner(toast, banner);
        if (g_menuOpen) DrawMenu();
        ImGui::Render();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        ctx->GSSetShader(gs, gsCI, gsN);
        SafeRelease(gs); for (UINT i = 0; i < gsN && i < 256; ++i) SafeRelease(gsCI[i]);
        saved->Restore(ctx);
        delete saved;
    } while (false);
    g_inDLAA = false;
    RepatchContext(ctx, "after UI");
    SafeRelease(rtv); SafeRelease(bb);
    SafeRelease(ctx); SafeRelease(dev);
}

// ================================================================== Present (매 프레임: 해제 -> 확인 -> 원본 -> 복구)
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain*, UINT, UINT);
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static uint64_t g_presentCount = 0;
static void* g_lastSC = nullptr;
static void* g_lastCtx = nullptr;


// ================================================================== Disable crash cams
// Same code patches as ZPatch's NoCrashCams: three conditional jumps in the game's camera code are made
// unconditional so the wreck camera is skipped. Takedown and spike strip cameras are not affected.
// The original bytes are kept, so the option can be turned on and off in game.
struct CodePatch {
    const char* name;
    const char* pattern;   // hex bytes, "?" = any
    int         offset;    // where to write, relative to the match
    int         len;
    uint8_t     bytes[8];  // patched bytes
    uint8_t*    addr;
    uint8_t     orig[8];
};
static CodePatch g_ccPatch[3] = {
    { "wreck cam A", "83 FA 04 75 12 0F 28",                3, 1, { 0xEB } },                               // jne -> jmp
    { "wreck cam B", "74 1C 48 8B 47 50 44 8B C5 0F 28 D6", 0, 1, { 0xEB } },                               // je  -> jmp
    { "wreck cam C", "0F 85 97 00 00 00 80 7E 54 00",       0, 6, { 0xE9, 0x98, 0x00, 0x00, 0x00, 0x90 } }, // jne -> jmp, nop
};

static int ParsePattern(const char* p, uint8_t* bytes, bool* mask, int max)
{
    int n = 0;
    while (*p && n < max) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') { mask[n] = false; bytes[n] = 0; ++n; while (*p == '?') ++p; continue; }
        unsigned v = 0;
        if (sscanf_s(p, "%2x", &v) != 1) break;
        bytes[n] = (uint8_t)v; mask[n] = true; ++n; p += 2;
    }
    return n;
}

// Scans one memory region. Runs under SEH: the game's protection layer can change page access while we read,
// and a fault here must never take the game down.
static int ScanRegion(uint8_t* a, uint8_t* last, const uint8_t* pb, const bool* pm, int pn, uint8_t** first)
{
    int found = 0;
    __try {
        for (uint8_t* c = a; c <= last; ) {
            c = (uint8_t*)memchr(c, pb[0], (size_t)(last - c) + 1);
            if (!c) break;
            int k = 1;
            while (k < pn && (!pm[k] || c[k] == pb[k])) ++k;
            if (k == pn) { if (++found == 1) *first = c; }
            ++c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return found;
}

// Searches the executable, readable memory of the game's own module. Returns the address if there is exactly one match.
static uint8_t* FindUnique(const char* pattern, int* count)
{
    uint8_t pb[64]; bool pm[64];
    const int pn = ParsePattern(pattern, pb, pm, 64);
    *count = 0;
    if (pn <= 0 || !pm[0]) return nullptr;
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)exe;
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    uint8_t* lo = (uint8_t*)exe;
    uint8_t* hi = lo + nt->OptionalHeader.SizeOfImage;
    uint8_t* found = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t* q = lo; q < hi && VirtualQuery(q, &mbi, sizeof(mbi)); q = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
        const DWORD pr = mbi.Protect & 0xFF;
        const bool exec = pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || !exec || (mbi.Protect & PAGE_GUARD)) continue;
        uint8_t* a = (uint8_t*)mbi.BaseAddress;
        uint8_t* e = std::min(hi, a + mbi.RegionSize);
        if (e - a < pn) continue;
        uint8_t* first = nullptr;
        const int n = ScanRegion(a, e - pn, pb, pm, pn, &first);
        if (n < 0) { Log("Crash cams: region %p could not be read, skipped", (void*)a); continue; }
        if (n > 0 && *count == 0) found = first;
        *count += n;
    }
    return *count == 1 ? found : nullptr;
}

static DWORD WINAPI CrashCamScanThread(LPVOID)
{
    bool ok = true;
    for (auto& cp : g_ccPatch) {
        int n = 0;
        uint8_t* m = FindUnique(cp.pattern, &n);
        if (!m) { Log("Crash cams: %s not found (%d matches)", cp.name, n); ok = false; continue; }
        cp.addr = m + cp.offset;
        memcpy(cp.orig, cp.addr, cp.len);
        Log("Crash cams: %s at exe+0x%llX", cp.name, (unsigned long long)(cp.addr - (uint8_t*)GetModuleHandleW(nullptr)));
    }
    if (!ok && GetModuleHandleW(L"ZPatchHP2010R.asi"))
        Log("Crash cams: ZPatch is loaded and may have patched this code already; use only one of the two NoCrashCams options");
    InterlockedExchange(&g_ccState, ok ? 2 : 3);
    return 0;
}

static void WriteCode(uint8_t* at, const uint8_t* src, int len)
{
    DWORD old = 0;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) { Log("Crash cams: VirtualProtect failed (%lu)", GetLastError()); return; }
    memcpy(at, src, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
}

// Called every Present: scans once in the background, then keeps the code in sync with the option
static void UpdateCrashCams()
{
    LONG st = g_ccState;
    if (st == 0) {
        if (!g_cfg.noCrashCams && !g_ccApplied) return;          // scan only when the option is used
        // Wait until the game has fully started (first TAA pass = 3D scene, or 30 s), so the
        // game's start-up code unpacking is finished before we read or patch its code
        static ULONGLONG t0 = 0;
        if (!t0) t0 = GetTickCount64();
        if (g_statTAABind == 0 && GetTickCount64() - t0 < 30000) return;
        InterlockedExchange(&g_ccState, 1);
        HANDLE t = CreateThread(nullptr, 0, CrashCamScanThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else InterlockedExchange(&g_ccState, 3);
        return;
    }
    if (st != 2 || g_ccApplied == g_cfg.noCrashCams) return;
    for (auto& cp : g_ccPatch) WriteCode(cp.addr, g_cfg.noCrashCams ? cp.bytes : cp.orig, cp.len);
    g_ccApplied = g_cfg.noCrashCams;
    Log("Crash cams %s", g_ccApplied ? "disabled" : "restored");
}

static void OnPresent(IDXGISwapChain* sc)
{
    ++g_presentCount;
    ++g_gameFrame;
    HandleInput();
    UpdateCrashCams();
    if (g_pendingTAA) { ++g_statMissed; g_pendingTAA = false; }
    g_doneThisFrame = false;
    if ((g_presentCount % 600) == 0)
        Log("Stats: present %llu | PSSetShader %ld | SRV sets %ld | TAA inputs %ld | TAA done %ld | done missed %ld | DLAA %llu | jitter pred %d/%d | enabled %d fatal %d",
            (unsigned long long)g_presentCount, g_statPS, g_statSRV, g_statTAABind, g_statTrigger, g_statMissed,
            (unsigned long long)g_frame, g_jHits, g_jChecks, g_cfg.enabled ? 1 : 0, g_fatal ? 1 : 0);
    if (sc == g_lastSC && (g_presentCount % 120) != 0) return;
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (ctx) {
            if (ctx != g_lastCtx) {
                Log("Game swapchain %p / context %p (present #%llu)", (void*)sc, (void*)ctx, (unsigned long long)g_presentCount);
                if (!g_lastCtx) g_bannerUntil = GetTickCount64() + 8000;   // 시작 안내 8초
                g_lastCtx = ctx;
            }
            PatchContext(ctx);
            RepatchContext(ctx, "Present check");
            ctx->Release();
        }
        dev->Release();
    }
    g_lastSC = sc;
}

// The game presents its start-up screen from a thread with a very small stack. Drawing the menu there
// (ImGui + the NVIDIA driver need a lot of stack) overflows it, so the overlay is skipped on such threads.
static bool StackRoomForUI()
{
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    volatile char probe = 0;
    const ULONG_PTR sp = (ULONG_PTR)&probe;
    const bool ok = sp > lo && sp - lo > 256 * 1024;
    static bool logged = false;
    if (!ok && !logged) { logged = true; Log("UI: skipped on a thread with only %llu KB of stack left (game start-up screen)", (unsigned long long)((sp - lo) >> 10)); }
    return ok;
}

// The hook stays in the vtable and calls the saved original directly. (Swapping the vtable slot out and
// back on every Present made the Steam overlay, which also hooks Present, re-hook itself on top of us and
// recurse until the stack overflowed when the ASI loader loaded this mod early.)
// Re-entrant calls (another overlay presenting through the vtable from inside our call) go straight through.
static thread_local int t_presentDepth = 0;
static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    HookedSC* h = FindSC(sc);
    if (!h || !h->Present) return DXGI_ERROR_INVALID_CALL;
    if (t_presentDepth > 0) return h->Present(sc, sync, flags);
    ++t_presentDepth;
    OnPresent(sc);
    if (StackRoomForUI()) DrawOverlay(sc);
    HRESULT hr = h->Present(sc, sync, flags);
    --t_presentDepth;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    HookedSC* h = FindSC(sc);
    if (!h || !h->Present1) return DXGI_ERROR_INVALID_CALL;
    if (t_presentDepth > 0) return h->Present1(sc, sync, flags, pp);
    ++t_presentDepth;
    OnPresent(sc);
    if (StackRoomForUI()) DrawOverlay(sc);
    HRESULT hr = h->Present1(sc, sync, flags, pp);
    --t_presentDepth;
    return hr;
}

static void DescribeAddr(void* a, char* out, size_t n);

// If another overlay (Steam) already replaced the vtable slot when we arrive, the slot value is that overlay's
// hook, and the overlay may later re-hook on top of us and call back into us. Calling its hook as our
// "original" then loops forever. So we read the slot's original value from the DLL file on disk (the vtable
// lives in that DLL's read-only data) and call the real DXGI function instead.
static void* OriginalFromDisk(void** vt, int idx)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(vt + idx), &m) || !m) return nullptr;
    wchar_t path[MAX_PATH]; if (!GetModuleFileNameW(m, path, MAX_PATH)) return nullptr;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    void* result = nullptr;
    const uint8_t* base = (const uint8_t*)m;
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const DWORD rva = (DWORD)((const uint8_t*)(vt + idx) - base);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (rva < sec->VirtualAddress || rva + 8 > sec->VirtualAddress + sec->SizeOfRawData) continue;
        LARGE_INTEGER off; off.QuadPart = sec->PointerToRawData + (rva - sec->VirtualAddress);
        // the image base in the file header is the preferred base the stored pointers are relative to
        IMAGE_DOS_HEADER fdos; IMAGE_NT_HEADERS64 fnt; uint64_t v = 0; DWORD rd = 0;
        LARGE_INTEGER z{}; 
        if (SetFilePointerEx(f, z, nullptr, FILE_BEGIN) && ReadFile(f, &fdos, sizeof(fdos), &rd, nullptr) && rd == sizeof(fdos)) {
            LARGE_INTEGER no; no.QuadPart = fdos.e_lfanew;
            if (SetFilePointerEx(f, no, nullptr, FILE_BEGIN) && ReadFile(f, &fnt, sizeof(fnt), &rd, nullptr) && rd == sizeof(fnt) &&
                SetFilePointerEx(f, off, nullptr, FILE_BEGIN) && ReadFile(f, &v, 8, &rd, nullptr) && rd == 8 && v > fnt.OptionalHeader.ImageBase) {
                const uint64_t fnRva = v - fnt.OptionalHeader.ImageBase;
                if (fnRva < nt->OptionalHeader.SizeOfImage) result = (void*)(base + fnRva);
            }
        }
        break;
    }
    CloseHandle(f);
    return result;
}

// Returns the function to call as "original" for a vtable slot
static void* RealOriginal(void** vt, int idx)
{
    void* cur = vt[idx];
    HMODULE mv = nullptr, mf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(vt + idx), &mv);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)cur, &mf);
    {
        char d[300]; DescribeAddr(cur, d, sizeof(d));
        const uint8_t* c = (const uint8_t*)cur; uint8_t fb[6] = {};
        __try { memcpy(fb, c, 6); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("Swapchain slot %d -> %s (first bytes %02X %02X %02X %02X %02X %02X)", idx, d, fb[0], fb[1], fb[2], fb[3], fb[4], fb[5]);
    }
    if (mv && mf == mv) return cur;   // slot still points into its own DLL: not hooked by anyone
    void* disk = OriginalFromDisk(vt, idx);
    char a[300], b[300]; DescribeAddr(cur, a, sizeof(a));
    if (disk) { DescribeAddr(disk, b, sizeof(b)); Log("Swapchain slot %d was already hooked (%s); calling the real function %s", idx, a, b); return disk; }
    Log("Swapchain slot %d was already hooked (%s) and the original could not be found", idx, a);
    return cur;
}

static void PatchSwapChain(IDXGISwapChain* sc)
{
    std::lock_guard<std::mutex> lk(g_vtLock);
    void** vt = *(void***)sc;
    for (int i = 0; i < g_scVtCount; ++i) if (g_scVt[i].vt == vt) return;
    if (g_scVtCount >= 4 || vt[Vtbl_Present()] == (void*)hkPresent) return;
    IDXGISwapChain1* sc1 = nullptr;
    bool has1 = SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1));
    SafeRelease(sc1);
    HookedSC& h = g_scVt[g_scVtCount];
    h.vt = vt;
    h.Present = (PFN_Present)RealOriginal(vt, Vtbl_Present());
    h.Present1 = has1 ? (PFN_Present1)RealOriginal(vt, Vtbl_Present1()) : nullptr;
    MemoryBarrier(); ++g_scVtCount; MemoryBarrier();
    WriteSlot(vt, Vtbl_Present(), (void*)hkPresent);
    if (has1) WriteSlot(vt, Vtbl_Present1(), (void*)hkPresent1);
    Log("Swapchain vtable patched (%p)", (void*)vt);
}

// True once the game's main window is visible (a top-level window of this process, at least 640x480)
static BOOL CALLBACK FindGameWindowCb(HWND w, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    RECT r; if (!GetClientRect(w, &r) || r.right - r.left < 640 || r.bottom - r.top < 480) return TRUE;
    *(bool*)lp = true; return FALSE;
}

static DWORD WINAPI InitThread(LPVOID)
{
    // Wait until the game window is up and the game has been presenting for a few seconds before hooking.
    // The Steam overlay sets up its own Present hooks during the game's first frames; if we hook before that
    // (a current Ultimate ASI Loader loads us very early) the two hooks end up calling each other until the
    // stack overflows. Older loaders loaded us later, which is why this did not show up before.
    {
        const ULONGLONG t0 = GetTickCount64();
        bool found = false;
        while (GetTickCount64() - t0 < 90000) {
            found = false;
            EnumWindows(FindGameWindowCb, (LPARAM)&found);
            if (found) break;
            Sleep(250);
        }
        Sleep(5000);
        Log("Hooking after %.1f s (%s)", (GetTickCount64() - t0) / 1000.0, found ? "game window ready" : "timed out waiting for the game window");
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NFS_DLAA_dummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("Dummy window creation failed"); return 0; }
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr)) PatchSwapChain(sc);   // 스왑체인 vtable 은 dxgi.dll 공용 -> 게임 스왑체인도 걸림
    else Log("Dummy swapchain creation failed 0x%08X", hr);
    // 더미 객체는 해제하지 않음
    return 0;
}

// ---------------------------------------------------------------- crash logging
static void DescribeAddr(void* a, char* out, size_t n)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        char path[MAX_PATH] = ""; GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        snprintf(out, n, "%s+0x%llX", base, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
    } else snprintf(out, n, "%p", a);
}
// Logs serious exceptions (only logs; the game or Windows still decides what happens)
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) > 6) return EXCEPTION_CONTINUE_SEARCH;
    char where[300]; DescribeAddr(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    Log("[exception] 0x%08X at %s (thread %lu)", code, where, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION)
        Log("[exception] %s address %p", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    uintptr_t* sp = (uintptr_t*)ep->ContextRecord->Rsp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        uintptr_t v = 0;
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        HMODULE m = nullptr;
        if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m) && m) {
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery((void*)v, &mi, sizeof(mi)) && (mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) {
                char d[300]; DescribeAddr((void*)v, d, sizeof(d));
                Log("[exception]   stack %d: %s", i, d);
                ++shown;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(mod, path, MAX_PATH);
        g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
        g_ini = g_dir + L"\\NFS_DLAA.ini";
        g_log = _wfsopen((g_dir + L"\\NFS_DLAA.log").c_str(), L"a", _SH_DENYNO);
        if (g_log) { fprintf(g_log, "\n"); fflush(g_log); }
        LoadConfig();
        AddVectoredExceptionHandler(1, CrashLogger);
        Log("NFS_DLAA v1.1 loaded (Enabled=%d Preset=%d Sharpness=%d NoCrashCams=%d Language=%s)", g_cfg.enabled, g_cfg.preset, (int)lroundf(g_cfg.sharpness * 100), g_cfg.noCrashCams ? 1 : 0, g_cfg.lang == 1 ? "ko" : "en");
        HANDLE th = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("Process exit (present %llu, DLAA frames %llu)", (unsigned long long)g_presentCount, (unsigned long long)g_frame);
    }
    return TRUE;
}
