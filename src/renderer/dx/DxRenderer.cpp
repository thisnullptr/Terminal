#include "precomp.h"

#include "DxRenderer.hpp"

#include "../../interactivity/win32/CustomWindowMessages.h"
#include "../../types/inc/Viewport.hpp"
#include "../../inc/unicode.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Render;

// Routine Description:
// - Constructs a DirectX-based renderer for console text
//   which primarily uses DirectWrite on a Direct2D surface
DxEngine::DxEngine() :
    RenderEngineBase(),
    _isInvalidUsed{ false },
    _invalidRect{ 0 },
    _invalidScroll{ 0 },
    _presentParams{ 0 },
    _presentReady{ false },
    _presentScroll{ 0 },
    _presentDirty{ 0 },
    _presentOffset{ 0 },
    _isEnabled{ false },
    _isPainting{ false },
    _displaySizePixels{ 0 },
    _fontSize{ 0 },
    _glyphCell{ 0 },
    _haveDeviceResources{ false },
    _hwndTarget((HWND)INVALID_HANDLE_VALUE)
{
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&_d2dFactory)));

    THROW_IF_FAILED(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(_dwriteFactory),
        reinterpret_cast<IUnknown **>(_dwriteFactory.GetAddressOf())
    ));
}

// Routine Description:
// - Destroys an instance of the DirectX rendering engine
DxEngine::~DxEngine()
{
    _ReleaseDeviceResources();
}

// Routine Description:
// - Sets this engine to enabled allowing painting and presentation to occur
// Arguments:
// - <none>
// Return Value:
// - Generally S_OK, but might return a DirectX or memory error if 
//   resources need to be created or adjusted when enabling to prepare for draw
//   Can give invalid state if you enable an enabled class.
[[nodiscard]]
HRESULT DxEngine::Enable() noexcept
{
    return _EnableDisplayAccess(true);
}

// Routine Description:
// - Sets this engine to disabled to prevent painting and presentation from occuring
// Arguments:
// - <none>
// Return Value:
// - Should be OK. We might close/free resources, but that shouldn't error.
//   Can give invalid state if you disable a disabled class.
[[nodiscard]]
HRESULT DxEngine::Disable() noexcept
{
    return _EnableDisplayAccess(false);
}

// Routine Description:
// - Helper to enable/disable painting/display access/presentation in a unified
//   manner between enable/disable functions.
// Arguments:
// - outputEnabled - true to enable, false to disable
// Return Value:
// - Generally OK. Can return invalid state if you set to the state that is already
//   active (enabling enabled, disabling disabled).
[[nodiscard]]
HRESULT DxEngine::_EnableDisplayAccess(const bool outputEnabled) noexcept
{
    // Invalid state if we're setting it to the same as what we already have.
    RETURN_HR_IF(E_NOT_VALID_STATE, outputEnabled == _isEnabled);

    _isEnabled = outputEnabled;
    if (!_isEnabled) {
        _ReleaseDeviceResources();
    }

    return S_OK;
}

// Routine Description;
// - Creates device-specific resources required for drawing
//   which generally means those that are represented on the GPU and can
//   vary based on the monitor, display adapter, etc.
// - These may need to be recreated during the course of painting a frame
//   should something about that hardware pipeline change.
// - Will free device resources that already existed as first operation.
// Arguments:
// - createSwapChain - If true, we create the entire rendering pipeline
//                   - If false, we just set up the adapter.
// Return Value:
// - Could be any DirectX/D3D/D2D/DXGI/DWrite error or memory issue.
[[nodiscard]]
HRESULT DxEngine::_CreateDeviceResources(const bool createSwapChain) noexcept
{
    if (_haveDeviceResources) 
    {
        _ReleaseDeviceResources();
    }

    auto freeOnFail = wil::scope_exit([&] { _ReleaseDeviceResources(); });

    RETURN_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&_dxgiFactory2)));

    RETURN_IF_FAILED(_dxgiFactory2->EnumAdapters1(0, &_dxgiAdapter1));

    const DWORD DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
#ifdef DBG
        D3D11_CREATE_DEVICE_DEBUG |
#endif
        D3D11_CREATE_DEVICE_SINGLETHREADED;

    D3D_FEATURE_LEVEL FeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1,
    };

    RETURN_IF_FAILED(D3D11CreateDevice(_dxgiAdapter1.Get(),
                                       D3D_DRIVER_TYPE_UNKNOWN,
                                       NULL,
                                       DeviceFlags,
                                       FeatureLevels,
                                       ARRAYSIZE(FeatureLevels),
                                       D3D11_SDK_VERSION,
                                       &_d3dDevice,
                                       NULL,
                                       &_d3dDeviceContext));

    RETURN_IF_FAILED(_dxgiAdapter1->EnumOutputs(0, &_dxgiOutput));

    _displaySizePixels = _GetClientSize();

    if (createSwapChain) {

        DXGI_SWAP_CHAIN_DESC1 SwapChainDesc = { 0 };
        SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        SwapChainDesc.BufferCount = 2;
        SwapChainDesc.SampleDesc.Count = 1;
        SwapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

        RECT rect = { 0 };
        RETURN_IF_WIN32_BOOL_FALSE(GetClientRect(_hwndTarget, &rect));

        SwapChainDesc.Width = rect.right - rect.left;
        SwapChainDesc.Height = rect.bottom - rect.top;

        RETURN_IF_FAILED(_dxgiFactory2->CreateSwapChainForHwnd(_d3dDevice.Get(),
                                                               _hwndTarget,
                                                               &SwapChainDesc,
                                                               nullptr,
                                                               nullptr,
                                                               &_dxgiSwapChain));

        RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(&_dxgiSurface)));

        D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
                0.0f,
                0.0f);

        RETURN_IF_FAILED(_d2dFactory->CreateDxgiSurfaceRenderTarget(_dxgiSurface.Get(),
                                                                    &props,
                                                                    &_d2dRenderTarget));

        _d2dRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        RETURN_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),
                                                                 &_d2dBrushBackground));

        RETURN_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                                                 &_d2dBrushForeground));
    }


    _haveDeviceResources = true;
    if (_isPainting) {
        // TODO: remove this or restore the "try a few times to render" code... I think
        _d2dRenderTarget->BeginDraw();
    }

    freeOnFail.release(); // don't need to release if we made it to the bottom and everything was good.

    return S_OK;
}

// Routine Description:
// - Releases device-specific resources (typically held on the GPU)
// Arguments:
// - <none>
// Return Value:
// - <none>
void DxEngine::_ReleaseDeviceResources() noexcept
{
    _haveDeviceResources = false;
    _d2dBrushForeground.Reset();
    _d2dBrushBackground.Reset();

    if (nullptr != _d2dRenderTarget.Get() && _isPainting)
    {
        _d2dRenderTarget->EndDraw();
    }

    _d2dRenderTarget.Reset();

    _dxgiSurface.Reset();
    _dxgiSwapChain.Reset();
    _dxgiOutput.Reset();

    if (nullptr != _d3dDeviceContext.Get())
    {
        // To ensure the swap chain goes away we must unbind any views from the
        // D3D pipeline
        _d3dDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    }
    _d3dDeviceContext.Reset();

    _d3dDevice.Reset();

    _dxgiAdapter1.Reset();
    _dxgiFactory2.Reset();
}

// Routine Description:
// - Helper to create a DirectWrite text layout object
//   out of a string.
// Arguments:
// - string - The text to attempt to layout
// - stringLength - Length of string above in characters
// - ppTextLayout - Location to receive new layout object
// Return Value:
// - S_OK if layout created successfully, otherwise a DirectWrite error
[[nodiscard]]
HRESULT DxEngine::_CreateTextLayout(
    _In_reads_(stringLength) PCWCHAR string,
    _In_ size_t stringLength,
    _Out_ IDWriteTextLayout **ppTextLayout) noexcept
{
    return _dwriteFactory->CreateTextLayout(string,
                                            static_cast<UINT32>(stringLength),
                                            _dwriteTextFormat.Get(),
                                            (float)_displaySizePixels.cx,
                                            _glyphCell.cy != 0 ? _glyphCell.cy : (float)_displaySizePixels.cy,
                                            ppTextLayout);
}

// Routine Description:
// - Sets the target window handle for our display pipeline
// - We will take over the surface of this window for drawing
// Arguments:
// - hwnd - Window handle
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::SetHwnd(const HWND hwnd) noexcept
{
    _hwndTarget = hwnd;
    return S_OK;
}

// Routine Description:
// - Invalidates a rectangle described in characters
// Arguments:
// - psrRegion - Character rectangle
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::Invalidate(const SMALL_RECT* const psrRegion) noexcept
{
    _InvalidOr(*psrRegion);
    return S_OK;
}

// Routine Description:
// - Invalidates one specific character coordinate
// Arguments:
// - pcoordCursor - single point in the character cell grid
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::InvalidateCursor(const COORD* const pcoordCursor) noexcept
{
    SMALL_RECT sr = Microsoft::Console::Types::Viewport::FromCoord(*pcoordCursor).ToInclusive();
    return Invalidate(&sr);
}

// Routine Description:
// - Invalidates a rectangle describing a pixel area on the display
// Arguments:
// - prcDirtyClient - pixel rectangle
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::InvalidateSystem(const RECT* const prcDirtyClient) noexcept
{
    _InvalidOr(*prcDirtyClient);

    return S_OK;
}

// Routine Description:
// - Invalidates a series of character rectangles 
// Arguments:
// - rectangles - One or more rectangles describing character positions on the grid
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::InvalidateSelection(const std::vector<SMALL_RECT>& rectangles) noexcept
{
    for (const auto& rect : rectangles)
    {
        RETURN_IF_FAILED(Invalidate(&rect));
    }
    return S_OK;
}

// Routine Description:
// - Scrolls the existing dirty region (if it exists) and
//   invalidates the area that is uncovered in the window.
// Arguments:
// - pcoordDelta - The number of characters to move and uncover.
//               - -Y is up, Y is down, -X is left, X is right.
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::InvalidateScroll(const COORD* const pcoordDelta) noexcept
{
    if (pcoordDelta->X != 0 || pcoordDelta->Y != 0)
    {
        POINT delta = { 0 };
        delta.x = pcoordDelta->X * _glyphCell.cx;
        delta.y = pcoordDelta->Y * _glyphCell.cy;

        _InvalidOffset(delta);

        _invalidScroll.cx += delta.x;
        _invalidScroll.cy += delta.y;

        // Add the revealed portion of the screen from the scroll to the invalid area.
        const RECT display = _GetDisplayRect();
        RECT reveal = display;

        // X delta first
        OffsetRect(&reveal, delta.x, 0);
        IntersectRect(&reveal, &reveal, &display);
        SubtractRect(&reveal, &display, &reveal);

        if (!IsRectEmpty(&reveal))
        {
            _InvalidOr(reveal);
        }

        // Y delta second (subtract rect won't work if you move both)
        reveal = display;
        OffsetRect(&reveal, 0, delta.y);
        IntersectRect(&reveal, &reveal, &display);
        SubtractRect(&reveal, &display, &reveal);

        if (!IsRectEmpty(&reveal))
        {
            _InvalidOr(reveal);
        }
    }

    return S_OK;
}

// Routine Description:
// - Invalidates the entire window area
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::InvalidateAll() noexcept
{
    const auto screen = _GetDisplayRect();
    _InvalidOr(screen);

    return S_OK;
}

// Routine Description:
// - This currently has no effect in this renderer.
// Arguments:
// - pForcePaint - Always filled with false
// Return Value:
// - S_FALSE because we don't use this.
[[nodiscard]]
HRESULT DxEngine::InvalidateCircling(_Out_ bool* const pForcePaint) noexcept
{
    *pForcePaint = false;
    return S_FALSE;
}

// Routine Description:
// - Gets the area in pixels of the surface we are targeting
// Arguments:
// - <none>
// Return Value:
// - X by Y area in pixels of the surface
[[nodiscard]]
SIZE DxEngine::_GetClientSize() const noexcept
{
    RECT clientRect = { 0 };
    LOG_IF_WIN32_BOOL_FALSE(GetClientRect(_hwndTarget, &clientRect));

    SIZE clientSize = { 0 };
    clientSize.cx = clientRect.right - clientRect.left;
    clientSize.cy = clientRect.bottom - clientRect.top;

    return clientSize;
}

// Routine Description:
// - Helper to multiply all parameters of a rectangle by the font size
//   to convert from characters to pixels.
// Arguments:
// - cellsToPixels - rectangle to update
// - fontSize - scaling factors
// Return Value:
// - <none> - Updates reference
void _ScaleByFont(RECT& cellsToPixels, SIZE fontSize) noexcept
{
    cellsToPixels.left *= fontSize.cx;
    cellsToPixels.right *= fontSize.cx;
    cellsToPixels.top *= fontSize.cy;
    cellsToPixels.bottom *= fontSize.cy;
}

// Routine Description:
// - Retrieves a rectangle representation of the pixel size of the
//   surface we are drawing on
// Arguments:
// - <none>
// Return Value;
// - Origin-placed rectangle representing the pixel size of the surface
[[nodiscard]]
RECT DxEngine::_GetDisplayRect() const noexcept
{
    return { 0, 0, _displaySizePixels.cx, _displaySizePixels.cy };
}

// Routine Description:
// - Helper to shift the existing dirty rectangle by a pixel offset
//   and crop it to still be within the bounds of the display surface
// Arguments:
// - delta - Adjustment distance in pixels
//         - -Y is up, Y is down, -X is left, X is right.
// Return Value:
// - <none>
void DxEngine::_InvalidOffset(POINT delta) noexcept
{
    if (_isInvalidUsed)
    {
        // Copy the existing invalid rect
        RECT invalidNew = _invalidRect;

        // Offset it to the new position
        THROW_IF_WIN32_BOOL_FALSE(OffsetRect(&invalidNew, delta.x, delta.y));

        // Get the rect representing the display
        const RECT rectScreen = _GetDisplayRect();

        // Ensure that the new invalid rectangle is still on the display
        IntersectRect(&invalidNew, &invalidNew, &rectScreen);

        _invalidRect = invalidNew;
    }
}

// Routine description:
// - Adds the given character rectangle to the total dirty region
// - Will scale internally to pixels based on the current font.
// Arguments:
// - sr - character rectangle
// Return Value:
// - <none>
void DxEngine::_InvalidOr(SMALL_RECT sr) noexcept
{
    RECT region;
    region.left = sr.Left;
    region.top = sr.Top;
    region.right = sr.Right;
    region.bottom = sr.Bottom;
    _ScaleByFont(region, _glyphCell);

    region.right += _glyphCell.cx;
    region.bottom += _glyphCell.cy;

    _InvalidOr(region);
}

// Routine Description:
// - Adds the given pixel rectangle to the total dirty region
// Arguments:
// - rc - Dirty pixel rectangle
// Return Value:
// - <none>
void DxEngine::_InvalidOr(RECT rc) noexcept
{
    if (_isInvalidUsed)
    {
        UnionRect(&_invalidRect, &_invalidRect, &rc);

        RECT rcScreen = _GetDisplayRect();
        IntersectRect(&_invalidRect, &_invalidRect, &rcScreen);
    }
    else
    {
        _invalidRect = rc;
        _isInvalidUsed = true;
    }
}

// Routine Description:
// - This is unused by this renderer.
// Arguments:
// - pForcePaint - always filled with false.
// Return Value:
// - S_FALSE because this is unused.
[[nodiscard]]
HRESULT DxEngine::PrepareForTeardown(_Out_ bool* const pForcePaint) noexcept
{
    *pForcePaint = false;
    return S_FALSE;
}

// Routine description:
// - Prepares the surfaces for painting and begins a drawing batch
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]]
HRESULT DxEngine::StartPaint() noexcept
{
    RETURN_HR_IF(E_HANDLE, _hwndTarget == INVALID_HANDLE_VALUE);
    RETURN_HR_IF(E_NOT_VALID_STATE, _isPainting); // invalid to start a paint while painting.

    if (_isEnabled) {
        const auto clientSize = _GetClientSize();
        if (!_haveDeviceResources ||
            _displaySizePixels.cy != clientSize.cy ||
            _displaySizePixels.cx != clientSize.cx) {
            RETURN_IF_FAILED(_CreateDeviceResources(true));
        }

        _d2dRenderTarget->BeginDraw();
        _isPainting = true;
    }

    return S_OK;
}

// Routine Description:
// - Ends batch drawing and captures any state necessary for presentation
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]]
HRESULT DxEngine::EndPaint() noexcept
{
    RETURN_HR_IF(E_INVALIDARG, !_isPainting); // invalid to end paint when we're not painting

    HRESULT hr = S_OK;

    if (_haveDeviceResources) {
        _isPainting = false;

        hr = _d2dRenderTarget->EndDraw();

        if (SUCCEEDED(hr)) {

            if (_invalidScroll.cy != 0 || _invalidScroll.cx != 0)
            {
                _presentDirty = _invalidRect;

                RECT display = _GetDisplayRect();
                SubtractRect(&_presentScroll, &display, &_presentDirty);
                _presentOffset.x = _invalidScroll.cx;
                _presentOffset.y = _invalidScroll.cy;

                _presentParams.DirtyRectsCount = 1;
                _presentParams.pDirtyRects = &_presentDirty;

                _presentParams.pScrollOffset = &_presentOffset;
                _presentParams.pScrollRect = &_presentScroll;

                if (IsRectEmpty(&_presentScroll))
                {
                    _presentParams.pScrollRect = nullptr;
                    _presentParams.pScrollOffset = nullptr;
                }
            }

            _presentReady = true;
        }
        else
        {
            _presentReady = false;
            _ReleaseDeviceResources();
        }
    }

    _invalidRect = { 0 };
    _isInvalidUsed = false;

    _invalidScroll = { 0 };

    return hr;
}

// Routine Description:
// - Copies the front surface of the swap chain (the one being displayed)
//   to the back surface of the swap chain (the one we draw on next)
//   so we can draw on top of what's already there.
// Arguments:
// - <none>
// Return Value:
// - Any DirectX error, a memory error, etc.
[[nodiscard]]
HRESULT DxEngine::_CopyFrontToBack() noexcept
{
    Microsoft::WRL::ComPtr<ID3D11Resource> backBuffer;
    Microsoft::WRL::ComPtr<ID3D11Resource> frontBuffer;

    RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    RETURN_IF_FAILED(_dxgiSwapChain->GetBuffer(1, IID_PPV_ARGS(&frontBuffer)));

    _d3dDeviceContext->CopyResource(backBuffer.Get(), frontBuffer.Get());

    return S_OK;
}

// Routine Description:
// - Takes queued drawing information and presents it to the screen.
// - This is separated out so it can be done outside the lock as it's expensive
// Arguments:
// - <none>
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]]
HRESULT DxEngine::Present() noexcept
{
    if (_presentReady)
    {
        FAIL_FAST_IF_FAILED(_dxgiSwapChain->Present1(1, 0, &_presentParams));

        RETURN_IF_FAILED(_CopyFrontToBack());
        _presentReady = false;

        _presentDirty = { 0 };
        _presentOffset = { 0 };
        _presentScroll = { 0 };
        _presentParams = { 0 };
    }

    return S_OK;
}

// Routine Description:
// - This is currently unused.
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::ScrollFrame() noexcept
{
    return S_OK;
}

// Routine Description:
// - This paints in the back most layer of the frame with the background color.
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::PaintBackground() noexcept
{
    _d2dRenderTarget->FillRectangle(D2D1::RectF((float)_invalidRect.left,
                                                (float)_invalidRect.top,
                                                (float)_invalidRect.right,
                                                (float)_invalidRect.bottom),
                                                _d2dBrushBackground.Get());

    return S_OK;
}

// Routine Description:
// - Places one line of text onto the screen at the given position
// Arguments:
// - pwsLine - The text
// - rgWidths - Width expected (in cells) of each character in the line
// - cchLine - Count of characters in the text
// - coord - Character coordinate position in the cell grid
// - fTrimLeft - Whether or not to trim off the left half of a double wide character
// - lineWrapped - Indicates that this line of text wrapped at the end of the row to the next line.
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]]
HRESULT DxEngine::PaintBufferLine(PCWCHAR const pwsLine,
                                  const unsigned char* const /*rgWidths*/,
                                  const size_t cchLine,
                                  const COORD coord,
                                  const bool /*fTrimLeft*/,
                                  const bool /*lineWrapped*/) noexcept
{
    try
    {
        // Calculate positioning of our origin and bounding rect.
        D2D1_POINT_2F origin;
        origin.x = static_cast<float>(coord.X * _glyphCell.cx);
        origin.y = static_cast<float>(coord.Y * _glyphCell.cy);

        D2D1_RECT_F rect = { 0 };
        rect.left = origin.x;
        rect.top = origin.y;
        rect.right = rect.left + (cchLine * _glyphCell.cx);
        rect.bottom = rect.top + _glyphCell.cy;

        // Draw background color first
        _d2dRenderTarget->FillRectangle(rect, _d2dBrushBackground.Get());

        // Now try to draw text on top.

        _glyphIds.assign(std::max(cchLine, static_cast<size_t>(1)), 0);

        BOOL isTextSimple;
        uint32_t textLengthRead;
        RETURN_IF_FAILED(_dwriteTextAnalyzer->GetTextComplexity(pwsLine, gsl::narrow<UINT32>(cchLine), _dwriteFontFace.Get(), &isTextSimple, &textLengthRead, &_glyphIds[0]));

        if (isTextSimple && textLengthRead == cchLine)
        {
            // This only provides marginal acceleration in tests 
            // It causes DrawGlyphRun to do GlyphPositionsFastPath instead of GlyphPositionsSlowPath)
            // which doesn't appear to save too much.
            // It could be optional.
            _glyphAdvances.assign(cchLine, static_cast<float>(_glyphCell.cx));

            DWRITE_GLYPH_RUN run = { 0 };
            run.fontEmSize = _fontSize;
            run.fontFace = _dwriteFontFace.Get();
            run.glyphCount = gsl::narrow<UINT32>(cchLine);
            run.glyphIndices = _glyphIds.data();
            run.glyphAdvances = _glyphAdvances.data(); // Could be optional, DrawGlyphRun will look it up if we don't give it.

            // Glyph runs take the origin as the baseline of the text, not the bounding box corner
            origin.y += _glyphCell.cy;
            origin.y -= _baseline * _glyphCell.cy;

            _d2dRenderTarget->DrawGlyphRun(origin, &run, _d2dBrushForeground.Get());
        }
        else
        {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            RETURN_IF_FAILED(_CreateTextLayout(pwsLine, cchLine, &layout));

            _d2dRenderTarget->DrawTextLayout(origin, layout.Get(), _d2dBrushForeground.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
    }
    CATCH_RETURN();

    return S_OK;
}

// Routine Description:
// - Paints lines around cells (draws in pieces of the grid)
// Arguments:
// - lines - Which grid lines (top, left, bottom, right) to draw
// - color - The color to use for drawing the lines
// - cchLine - Length of the line to draw in character cells
// - coordTarget - The X,Y character position in the grid where we should start drawing 
//               - We will draw rightward (+X) from here
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]]
HRESULT DxEngine::PaintBufferGridLines(GridLines const lines,
                                       COLORREF const color,
                                       size_t const cchLine,
                                       COORD const coordTarget) noexcept
{
    const auto existingColor = _d2dBrushForeground->GetColor();
    const auto restoreBrushOnExit = wil::scope_exit([&] {_d2dBrushForeground->SetColor(existingColor); });

    _d2dBrushForeground->SetColor(D2D1::ColorF(color));

    const auto font = _GetFontSize();
    D2D_POINT_2F target;
    target.x = static_cast<float>(coordTarget.X) * font.X;
    target.y = static_cast<float>(coordTarget.Y) * font.Y;

    D2D_POINT_2F start = { 0 };
    D2D_POINT_2F end = { 0 };

    for (size_t i = 0; i < cchLine; i++)
    {
        start = target;

        if (lines & GridLines::Top)
        {
            end = start;
            end.x += font.X;

            _d2dRenderTarget->DrawLine(start, end, _d2dBrushForeground.Get());
        }

        if (lines & GridLines::Left)
        {
            end = start;
            end.y += font.Y;

            _d2dRenderTarget->DrawLine(start, end, _d2dBrushForeground.Get());
        }

        // NOTE: Watch out for inclusive/exclusive rectangles here.
        // We have to remove 1 from the font size for the bottom and right lines to ensure that the
        // starting point remains within the clipping rectangle.
        // For example, if we're drawing a letter at 0,0 and the font size is 8x16....
        // The bottom left corner inclusive is at 0,15 which is Y (0) + Font Height (16) - 1 = 15.
        // The top right corner inclusive is at 7,0 which is X (0) + Font Height (8) - 1 = 7.

        start = target;
        start.y += font.Y - 1;

        if (lines & GridLines::Bottom)
        {
            end = start;
            end.x += font.X;

            _d2dRenderTarget->DrawLine(start, end, _d2dBrushForeground.Get());
        }

        start = target;
        start.x += font.X - 1;

        if (lines & GridLines::Right)
        {
            end = start;
            end.y += font.Y;

            _d2dRenderTarget->DrawLine(start, end, _d2dBrushForeground.Get());
        }

        // Move to the next character in this run.
        target.x += font.X;
    }

    return S_OK;
}

// Routine Description:
// - Paints an overlay highlight on a portion of the frame to represent selected text
// Arguments:
//  - rect - Rectangle to invert or highlight to make the selection area
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]]
HRESULT DxEngine::PaintSelection(const SMALL_RECT rect) noexcept
{
    const auto existingColor = _d2dBrushForeground->GetColor();
    const auto selectionColor = D2D1::ColorF(existingColor.r,
                                             existingColor.g,
                                             existingColor.b,
                                             0.5f);

    _d2dBrushForeground->SetColor(selectionColor);
    const auto resetColorOnExit = wil::scope_exit([&] {_d2dBrushForeground->SetColor(existingColor); });

    RECT pixels;
    pixels.left = rect.Left * _glyphCell.cx;
    pixels.top = rect.Top * _glyphCell.cy;
    pixels.right = rect.Right * _glyphCell.cx;
    pixels.bottom = rect.Bottom * _glyphCell.cy;

    D2D1_RECT_F draw = { 0 };
    draw.left = static_cast<float>(pixels.left);
    draw.top = static_cast<float>(pixels.top);
    draw.right = static_cast<float>(pixels.right);
    draw.bottom = static_cast<float>(pixels.bottom);

    _d2dRenderTarget->FillRectangle(draw, _d2dBrushForeground.Get());

    return S_OK;
}

// Helper to choose which Direct2D method to use when drawing the cursor rectangle
enum class CursorPaintType
{
    Fill,
    Outline
};

// Routine Description:
// - Draws a block at the given position to represent the cursor
// - May be a styled cursor at the character cell location that is less than a full block
// Arguments:
// - coordCursor - Character cell in the grid to draw at
// - ulCursorHeightPercent - For an underscore type _ cursor, how tall it should be as a % of the cell height
// - fIsDoubleWidth - Whether to draw the cursor 2 cells wide (+X from the coordinate given)
// - cursorType - Chooses a special cursor type like a full box, a vertical bar, etc.
// - fUseColor - Specifies to use the color below instead of the default color
// - cursorColor - Color to use for drawing instead of the default
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]]
HRESULT DxEngine::PaintCursor(const COORD coordCursor,
                              const ULONG ulCursorHeightPercent,
                              const bool fIsDoubleWidth,
                              const CursorType cursorType,
                              const bool fUseColor,
                              const COLORREF cursorColor) noexcept
{
    // Create rectangular block representing where the cursor can fill.
    D2D1_RECT_F rect = { 0 };
    rect.left = static_cast<float>(coordCursor.X * _glyphCell.cx);
    rect.top = static_cast<float>(coordCursor.Y * _glyphCell.cy);
    rect.right = static_cast<float>(rect.left + _glyphCell.cx);
    rect.bottom = static_cast<float>(rect.top + _glyphCell.cy);

    // If we're double-width, make it one extra glyph wider
    if (fIsDoubleWidth)
    {
        rect.right += _glyphCell.cx;
    }

    CursorPaintType paintType = CursorPaintType::Fill;

    switch (cursorType)
    {
    case CursorType::Legacy:
    {
        // Enforce min/max cursor height
        ULONG ulHeight = std::clamp(ulCursorHeightPercent, s_ulMinCursorHeightPercent, s_ulMaxCursorHeightPercent);
        ulHeight = (ULONG)((_glyphCell.cy * ulHeight) / 100);
        rect.top = rect.bottom - ulHeight;
        break;
    }
    case CursorType::VerticalBar:
    {
        rect.right = rect.left + 1;
        break;
    }
    case CursorType::Underscore:
    {
        rect.top = rect.bottom - 1;
        break;
    }
    case CursorType::EmptyBox:
    {
        paintType = CursorPaintType::Outline;
        break;
    }
    case CursorType::FullBox:
    {
        break;
    }
    default:
        return E_NOTIMPL;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush = _d2dBrushForeground;

    if (fUseColor)
    {
        RETURN_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(D2D1::ColorF(cursorColor), &brush));
    }

    switch (paintType)
    {
    case CursorPaintType::Fill:
    {
        _d2dRenderTarget->FillRectangle(rect, brush.Get());
        break;
    }
    case CursorPaintType::Outline:
    {
        _d2dRenderTarget->DrawRectangle(rect, brush.Get());
        break;
    }
    default:
        return E_NOTIMPL;
    }

    return S_OK;
}

// Routine Description:
// - Unused in this renderer
// Arguments:
// - <none>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::ClearCursor() noexcept
{
    return S_OK;
}

// Routine Description:
// - Updates the default brush colors used for drawing
// Arguments:
// - colorForeground - Foreground brush color
// - colorBackground - Background brush color
// - legacyColorAttribute - <unused> 
// - isBold - <unused> 
// - fIncludeBackgrounds - <unused>
// Return Value:
// - S_OK or relevant DirectX error.
[[nodiscard]]
HRESULT DxEngine::UpdateDrawingBrushes(COLORREF const colorForeground,
                                       COLORREF const colorBackground,
                                       const WORD /*legacyColorAttribute*/,
                                       const bool /*isBold*/,
                                       bool const /*fIncludeBackgrounds*/) noexcept
{
    _d2dBrushForeground->SetColor(s_ColorFFromColorRef(colorForeground));
    _d2dBrushBackground->SetColor(s_ColorFFromColorRef(colorBackground));

    return S_OK;
}

// Routine Description:
// - Updates the font used for drawing
// Arguments:
// - pfiFontInfoDesired - Information specifying the font that is requested
// - fiFontInfo - Filled with the nearest font actually chosen for drawing
// Return Value:
// - S_OK or relevant DirectX error
[[nodiscard]]
HRESULT DxEngine::UpdateFont(const FontInfoDesired& pfiFontInfoDesired, FontInfo& fiFontInfo) noexcept
{
    try
    {
        const std::wstring fontName(pfiFontInfoDesired.GetFaceName());
        const DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        const DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
        const DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;

        const auto fontFace = _FindFontFace(fontName, weight, stretch, style);

        DWRITE_FONT_METRICS1 fontMetrics;
        fontFace->GetMetrics(&fontMetrics);

        _baseline = static_cast<float>(fontMetrics.descent) / fontMetrics.designUnitsPerEm;
        const UINT32 spaceCodePoint = UNICODE_SPACE;
        UINT16 spaceGlyphIndex;
        THROW_IF_FAILED(fontFace->GetGlyphIndicesW(&spaceCodePoint, 1, &spaceGlyphIndex));

        INT32 advanceInDesignUnits;
        THROW_IF_FAILED(fontFace->GetDesignGlyphAdvances(1, &spaceGlyphIndex, &advanceInDesignUnits));

        const float heightDesired = static_cast<float>(pfiFontInfoDesired.GetEngineSize().Y);
        const float widthAdvance = static_cast<float>(advanceInDesignUnits) / fontMetrics.designUnitsPerEm;
        const float widthApprox = heightDesired * widthAdvance;
        const float widthExact = round(widthApprox);
        const float fontSize = widthExact / widthAdvance;

        const auto lineSpacing = s_DetermineLineSpacing(fontFace.Get(), fontSize, ceilf(fontSize));

        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        THROW_IF_FAILED(_dwriteFactory->CreateTextFormat(fontName.data(),
                                                         nullptr,
                                                         weight,
                                                         style,
                                                         stretch,
                                                         fontSize,
                                                         L"",
                                                         &format));

        THROW_IF_FAILED(format.As(&_dwriteTextFormat));

        Microsoft::WRL::ComPtr<IDWriteTextAnalyzer> analyzer;
        THROW_IF_FAILED(_dwriteFactory->CreateTextAnalyzer(&analyzer));
        THROW_IF_FAILED(analyzer.As(&_dwriteTextAnalyzer));

        _dwriteFontFace = fontFace;

        THROW_IF_FAILED(_dwriteTextFormat->SetLineSpacing(&lineSpacing));
        THROW_IF_FAILED(_dwriteTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR));
        THROW_IF_FAILED(_dwriteTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));

        _glyphCell.cx = gsl::narrow<LONG>(widthExact);
        _glyphCell.cy = gsl::narrow<LONG>(ceilf(fontSize));

        _fontSize = fontSize;

        COORD coordSize = { 0 };
        THROW_IF_FAILED(GetFontSize(&coordSize));

        const auto familyNameLength = _dwriteTextFormat->GetFontFamilyNameLength() + 1; // 1 for space for null
        const auto familyNameBuffer = std::make_unique<wchar_t[]>(familyNameLength);
        THROW_IF_FAILED(_dwriteTextFormat->GetFontFamilyName(familyNameBuffer.get(), familyNameLength));

        const DWORD weightDword = static_cast<DWORD>(_dwriteTextFormat->GetFontWeight());

        fiFontInfo.SetFromEngine(familyNameBuffer.get(),
                                 fiFontInfo.GetFamily(),
                                 weightDword,
                                 true,
                                 coordSize,
                                 coordSize);

    }
    CATCH_RETURN();

    return S_OK;
}

// Routine Description:
// - Not currently used by this renderer.
// Arguments:
// - iDpi - <unused>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::UpdateDpi(int const /*iDpi*/) noexcept
{
    return S_OK;
}

// Method Description:
// - This method will update our internal reference for how big the viewport is.
//      Does nothing for DX.
// Arguments:
// - srNewViewport - The bounds of the new viewport.
// Return Value:
// - HRESULT S_OK
[[nodiscard]]
HRESULT DxEngine::UpdateViewport(const SMALL_RECT /*srNewViewport*/) noexcept
{
    return S_OK;
}

// Routine Description:
// - Currently unused by this renderer
// Arguments:
// - pfiFontInfoDesired - <unused>
// - pfiFontInfo - <unused>
// - iDpi - <unused>
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::GetProposedFont(const FontInfoDesired& /*pfiFontInfoDesired*/,
                                  FontInfo& /*pfiFontInfo*/,
                                  int const /*iDpi*/) noexcept
{
    return S_OK;
}

// Routine Description:
// - Gets the area that we currently believe is dirty within the character cell grid
// Arguments:
// - <none>
// Return Value:
// - Rectangle describing dirty area in characters.
[[nodiscard]]
SMALL_RECT DxEngine::GetDirtyRectInChars() noexcept
{
    SMALL_RECT r;
    r.Top = (SHORT)(floor(_invalidRect.top / _glyphCell.cy));
    r.Left = (SHORT)(floor(_invalidRect.left / _glyphCell.cx));
    r.Bottom = (SHORT)(floor(_invalidRect.bottom / _glyphCell.cy));
    r.Right = (SHORT)(floor(_invalidRect.right / _glyphCell.cx));

    // Exclusive to inclusive
    r.Bottom--;
    r.Right--;

    return r;
}

// Routine Description:
// - Gets COORD packed with shorts of each glyph (character) cell's
//   height and width. 
// Arguments:
// - <none>
// Return Value:
// - Nearest integer short x and y values for each cell.
[[nodiscard]]
COORD DxEngine::_GetFontSize() const noexcept
{
    return { (SHORT)(_glyphCell.cx), (SHORT)(_glyphCell.cy) };
}

// Routine Description:
// - Gets the current font size 
// Arguments:
// - pFontSize - Filled with the font size.
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::GetFontSize(_Out_ COORD* const pFontSize) noexcept
{
    *pFontSize = _GetFontSize();
    return S_OK;
}

// Routine Description:
// - Currently unused by this renderer.
// Arguments:
// - glyph - <unused>
// - pResult - Filled with false.
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::IsGlyphWideByFont(const std::wstring_view /*glyph*/, _Out_ bool* const pResult) noexcept
{
    *pResult = false;
    return S_OK;
}

// Method Description:
// - Updates the window's title string.
// Arguments:
// - newTitle: the new string to use for the title of the window
// Return Value:
// - S_OK
[[nodiscard]]
HRESULT DxEngine::_DoUpdateTitle(_In_ const std::wstring& /*newTitle*/) noexcept
{
    return PostMessageW(_hwndTarget, CM_UPDATE_TITLE, 0, (LPARAM)nullptr) ? S_OK : E_FAIL;
}

// Routine Description:
// - Locates a suitable font face from the given information
// Arguments:
// - familyName - The font name we should be looking for
// - weight - The weight (bold, light, etc.)
// - stretch - The stretch of the font is the spacing between each letter
// - style - Normal, italic, etc.
// Return Value:
// - Smart pointer holding interface reference for queryable font data.
[[nodiscard]]
Microsoft::WRL::ComPtr<IDWriteFontFace5> DxEngine::_FindFontFace(const std::wstring& familyName,
                                                                 DWRITE_FONT_WEIGHT weight,
                                                                 DWRITE_FONT_STRETCH stretch,
                                                                 DWRITE_FONT_STYLE style)
{
    Microsoft::WRL::ComPtr<IDWriteFontFace5> fontFace;

    Microsoft::WRL::ComPtr<IDWriteFontCollection> fontCollection;
    THROW_IF_FAILED(_dwriteFactory->GetSystemFontCollection(&fontCollection, false));

    UINT32 familyIndex;
    BOOL familyExists;
    THROW_IF_FAILED(fontCollection->FindFamilyName(familyName.c_str(), &familyIndex, &familyExists));

    if (familyExists)
    {
        Microsoft::WRL::ComPtr<IDWriteFontFamily> fontFamily;
        THROW_IF_FAILED(fontCollection->GetFontFamily(familyIndex, &fontFamily));

        Microsoft::WRL::ComPtr<IDWriteFont> font;
        THROW_IF_FAILED(fontFamily->GetFirstMatchingFont(weight, stretch, style, &font));

        Microsoft::WRL::ComPtr<IDWriteFontFace> fontFace0;
        THROW_IF_FAILED(font->CreateFontFace(&fontFace0));

        THROW_IF_FAILED(fontFace0.As(&fontFace));
    }

    return fontFace;
}

// Routine Description:
// - Calculate the line spacing information necessary to place the floating point size font
//   into an integer size vertical spacing between lines
// Arguments:
// - fontFace - Interface to queryable font information
// - fontSize - Floating point font size that will be used to draw
// - cellHeight - The exact height desired in pixels that each character should take in the screen
// Return Value:
// - Structure containing line spacing data that can be given to DWrite when drawing.
[[nodiscard]]
DWRITE_LINE_SPACING DxEngine::s_DetermineLineSpacing(IDWriteFontFace5* const fontFace, const float fontSize, const float cellHeight) noexcept
{
    DWRITE_FONT_METRICS1 fontMetrics;
    fontFace->GetMetrics(&fontMetrics);
    const float ascent = (fontSize * fontMetrics.ascent) / fontMetrics.designUnitsPerEm;
    const float descent = (fontSize * fontMetrics.descent) / fontMetrics.designUnitsPerEm;

    DWRITE_LINE_SPACING result = {};
    result.method = DWRITE_LINE_SPACING_METHOD_UNIFORM;
    result.height = cellHeight;
    result.baseline = ascent + (result.height - (ascent + descent)) / 2;

    return result;
}

// Routine Description:
// - Helps convert a GDI COLORREF into a Direct2D ColorF
// Arguments:
// - color - GDI color
// Return Value:
// - D2D color
[[nodiscard]]
D2D1_COLOR_F DxEngine::s_ColorFFromColorRef(const COLORREF color) noexcept
{
    // Converts BGR color order to RGB.
    const UINT32 rgb = ((color & 0x0000FF) << 16) | (color & 0x00FF00) | ((color & 0xFF0000) >> 16);

    return D2D1::ColorF(rgb);
}