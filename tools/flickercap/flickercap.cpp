// flickercap: records what the display really shows (DXGI Desktop Duplication) and reports frames
// that differ from both neighbours while the neighbours agree with each other (A-B-A flicker).
//
//   flickercap <seconds> <out dir> [region threshold, default 10]
//
// Writes <out dir>\frames.csv (one line per desktop update) and, per detected flicker, a sheet
// <out dir>\flicker_<index>.bmp: the three frames at quarter resolution (top) and a 512x512
// full-resolution crop from the screen centre of each (bottom), left to right = before, flicker, after.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Frame {
    int index = 0;
    double t = 0;          // ms since start
    unsigned acc = 0;      // desktop updates folded into this one (1 = none missed)
    int slot = 0;          // history slot
    std::vector<uint8_t> lum;
    SYSTEMTIME wall{};
};

void WriteBmp(const std::wstring& path, int w, int h, const std::vector<uint8_t>& bgraTopDown)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)bgraTopDown.size();
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgraTopDown.data(), 1, bgraTopDown.size(), f);
    fclose(f);
}

ComPtr<ID3D11Texture2D> MakeTex(ID3D11Device* dev, int w, int h, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc, UINT mips)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = mips;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    d.MiscFlags = misc;
    ComPtr<ID3D11Texture2D> t;
    dev->CreateTexture2D(&d, nullptr, &t);
    return t;
}

// Copies a staging texture into dst at (x0, y0) of a top-down BGRA image of width dw.
void Blit(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging, int w, int h, std::vector<uint8_t>& dst, int dw, int x0, int y0)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return;
    for (int y = 0; y < h; ++y)
        memcpy(&dst[((size_t)(y0 + y) * dw + x0) * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    ctx->Unmap(staging, 0);
}

}   // namespace

int wmain(int argc, wchar_t** argv)
{
    const double seconds = argc > 1 ? _wtof(argv[1]) : 30.0;
    const std::wstring out = argc > 2 ? argv[2] : L".";
    const float thr = argc > 3 ? (float)_wtof(argv[3]) : 10.0f;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    CreateDirectoryW(out.c_str(), nullptr);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIAdapter1> ad;
    for (UINT a = 0; factory->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 adesc{};
        ad->GetDesc1(&adesc);
        ComPtr<IDXGIOutput> op;
        for (UINT o = 0; ad->EnumOutputs(o, &op) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            op->GetDesc(&od);
            wprintf(L"adapter %u (%s) output %u %s: (%ld,%ld)-(%ld,%ld)%s\n", a, adesc.Description, o, od.DeviceName,
                    od.DesktopCoordinates.left, od.DesktopCoordinates.top, od.DesktopCoordinates.right,
                    od.DesktopCoordinates.bottom, od.AttachedToDesktop ? L"" : L" (not attached)");
            if (!output && od.AttachedToDesktop && od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0) {
                adapter = ad;
                output = op;
            }
        }
    }
    if (!output) {
        wprintf(L"no output at (0,0)\n");
        return 1;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        wprintf(L"D3D11CreateDevice failed\n");
        return 1;
    }
    auto duplicate = [&](ComPtr<IDXGIOutputDuplication>& dup) -> HRESULT {
        dup.Reset();
        ComPtr<IDXGIOutput5> o5;
        HRESULT hr = E_NOINTERFACE;
        if (SUCCEEDED(output.As(&o5))) {
            const DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            hr = o5->DuplicateOutput1(dev.Get(), 0, 1, fmts, &dup);
        }
        if (FAILED(hr)) {
            ComPtr<IDXGIOutput1> o1;
            if (SUCCEEDED(output.As(&o1))) hr = o1->DuplicateOutput(dev.Get(), &dup);
        }
        return hr;
    };
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = duplicate(dup);
    if (FAILED(hr)) {
        wprintf(L"DuplicateOutput failed 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup->GetDesc(&dd);
    const int W = (int)dd.ModeDesc.Width, H = (int)dd.ModeDesc.Height;
    wprintf(L"capturing %dx%d @ %u/%u Hz for %.0f s -> %s\n", W, H, dd.ModeDesc.RefreshRate.Numerator,
            dd.ModeDesc.RefreshRate.Denominator, seconds, out.c_str());

    ComPtr<ID3D11Texture2D> full = MakeTex(dev.Get(), W, H, D3D11_USAGE_DEFAULT,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0,
                                           D3D11_RESOURCE_MISC_GENERATE_MIPS, 0);
    if (!full) return 1;
    D3D11_TEXTURE2D_DESC fd{};
    full->GetDesc(&fd);
    ComPtr<ID3D11ShaderResourceView> srv;
    dev->CreateShaderResourceView(full.Get(), nullptr, &srv);
    int level = 0, lw = W, lh = H;
    while (lw > 160) {
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
        level++;
    }
    const int qw = std::max(1, W >> 2), qh = std::max(1, H >> 2), crop = std::min(512, std::min(W, H));
    ComPtr<ID3D11Texture2D> tiny = MakeTex(dev.Get(), lw, lh, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 1);
    ComPtr<ID3D11Texture2D> hist[4], crops[4];
    for (int i = 0; i < 4; ++i) {
        hist[i] = MakeTex(dev.Get(), qw, qh, D3D11_USAGE_DEFAULT, 0, 0, 0, 1);
        crops[i] = MakeTex(dev.Get(), crop, crop, D3D11_USAGE_DEFAULT, 0, 0, 0, 1);
    }
    ComPtr<ID3D11Texture2D> histStage = MakeTex(dev.Get(), qw, qh, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 1);
    ComPtr<ID3D11Texture2D> cropStage = MakeTex(dev.Get(), crop, crop, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 1);

    FILE* csv = _wfopen((out + L"\\frames.csv").c_str(), L"w");
    if (csv) fprintf(csv, "index,wall,t_ms,dt_ms,accumulated,mean_luma,diff_prev\n");
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    std::vector<Frame> win;
    int index = 0, events = 0, sheets = 0, timeouts = 0, missed = 0, lost = 0;
    double lastT = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double tms = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (tms > seconds * 1000.0) break;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(500, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            timeouts++;
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            lost++;
            SYSTEMTIME st;
            GetLocalTime(&st);
            wprintf(L"%02d:%02d:%02d.%03d duplication lost (0x%08lx), recreating\n", st.wHour, st.wMinute, st.wSecond,
                    st.wMilliseconds, (unsigned long)hr);
            Sleep(100);
            duplicate(dup);
            if (!dup) Sleep(500);
            win.clear();
            continue;
        }
        if (FAILED(hr)) {
            wprintf(L"AcquireNextFrame failed 0x%08lx\n", (unsigned long)hr);
            Sleep(200);
            duplicate(dup);
            win.clear();
            continue;
        }
        if (fi.LastPresentTime.QuadPart == 0) {   // only the mouse pointer changed
            dup->ReleaseFrame();
            continue;
        }
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        const int slot = index % 4;
        ctx->CopySubresourceRegion(full.Get(), 0, 0, 0, 0, tex.Get(), 0, nullptr);
        D3D11_BOX box{(UINT)(W - crop) / 2, (UINT)(H - crop) / 2, 0, (UINT)(W - crop) / 2 + crop, (UINT)(H - crop) / 2 + crop, 1};
        ctx->CopySubresourceRegion(crops[slot].Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
        dup->ReleaseFrame();
        ctx->GenerateMips(srv.Get());
        ctx->CopySubresourceRegion(hist[slot].Get(), 0, 0, 0, 0, full.Get(), D3D11CalcSubresource(2, 0, fd.MipLevels), nullptr);
        ctx->CopySubresourceRegion(tiny.Get(), 0, 0, 0, 0, full.Get(), D3D11CalcSubresource(level, 0, fd.MipLevels), nullptr);
        Frame f;
        f.index = index;
        f.slot = slot;
        f.acc = fi.AccumulatedFrames;
        GetLocalTime(&f.wall);
        QueryPerformanceCounter(&now);
        f.t = (double)(now.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        f.lum.resize((size_t)lw * lh);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(tiny.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < lh; ++y) {
                const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (int x = 0; x < lw; ++x) f.lum[(size_t)y * lw + x] = (uint8_t)((p[x * 4] + 2 * p[x * 4 + 1] + p[x * 4 + 2]) / 4);
            }
            ctx->Unmap(tiny.Get(), 0);
        }
        double mean = 0, diff = 0;
        for (size_t i = 0; i < f.lum.size(); ++i) {
            mean += f.lum[i];
            if (!win.empty()) diff += std::abs((int)f.lum[i] - (int)win.back().lum[i]);
        }
        mean /= (double)f.lum.size();
        diff /= (double)f.lum.size();
        if (f.acc > 1) missed += (int)f.acc - 1;
        if (csv)
            fprintf(csv, "%d,%02d:%02d:%02d.%03d,%.1f,%.1f,%u,%.2f,%.2f\n", index, f.wall.wHour, f.wall.wMinute, f.wall.wSecond,
                    f.wall.wMilliseconds, f.t, f.t - lastT, f.acc, mean, diff);
        lastT = f.t;
        win.push_back(std::move(f));
        if (win.size() > 3) win.erase(win.begin());
        index++;
        if (win.size() < 3 || win[1].acc > 1 || win[2].acc > 1) continue;
        const Frame &A = win[0], &B = win[1], &C = win[2];
        const int gx = 8, gy = 6;
        int count = 0;
        float wab = 0, wbc = 0, wac = 0;
        for (int ry = 0; ry < gy; ++ry)
            for (int rx = 0; rx < gx; ++rx) {
                const int x0 = rx * lw / gx, x1 = (rx + 1) * lw / gx, y0 = ry * lh / gy, y1 = (ry + 1) * lh / gy;
                long ab = 0, bc = 0, ac = 0, n = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x, ++n) {
                        const size_t i = (size_t)y * lw + x;
                        ab += std::abs((int)A.lum[i] - (int)B.lum[i]);
                        bc += std::abs((int)B.lum[i] - (int)C.lum[i]);
                        ac += std::abs((int)A.lum[i] - (int)C.lum[i]);
                    }
                if (!n) continue;
                const float dab = (float)ab / n, dbc = (float)bc / n, dac = (float)ac / n;
                if (dab > thr && dbc > thr && dac < 0.4f * std::min(dab, dbc)) {
                    count++;
                    if (dab + dbc > wab + wbc) {
                        wab = dab;
                        wbc = dbc;
                        wac = dac;
                    }
                }
            }
        if (!count) continue;
        events++;
        wprintf(L"%02d:%02d:%02d.%03d FLICKER update %d (%.1f ms after the previous, next after %.1f ms): %d/%d regions "
                L"(A>B %.0f, B>C %.0f, A>C %.0f)\n",
                B.wall.wHour, B.wall.wMinute, B.wall.wSecond, B.wall.wMilliseconds, B.index, B.t - A.t, C.t - B.t, count,
                gx * gy, wab, wbc, wac);
        if (sheets >= 16) continue;
        const int sw = 3 * qw, sh = qh + crop;
        std::vector<uint8_t> sheet((size_t)sw * sh * 4, 0);
        const Frame* fr[3] = {&A, &B, &C};
        for (int i = 0; i < 3; ++i) {
            ctx->CopyResource(histStage.Get(), hist[fr[i]->slot].Get());
            Blit(ctx.Get(), histStage.Get(), qw, qh, sheet, sw, i * qw, 0);
            ctx->CopyResource(cropStage.Get(), crops[fr[i]->slot].Get());
            if (crop <= qw) Blit(ctx.Get(), cropStage.Get(), crop, crop, sheet, sw, i * qw, qh);
        }
        wchar_t name[64];
        swprintf_s(name, L"\\flicker_%05d.bmp", B.index);
        WriteBmp(out + name, sw, sh, sheet);
        sheets++;
    }
    if (csv) fclose(csv);
    wprintf(L"done: %d desktop updates, %d missed, %d timeouts, %d lost, %d A-B-A flickers\n", index, missed, timeouts, lost, events);
    return 0;
}
