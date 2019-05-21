//
// Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Common.h"
#include "D3D12MemAlloc.h"

const wchar_t * const CLASS_NAME = L"D3D12MemAllocSample";
const wchar_t * const WINDOW_TITLE = L"Direct3D 12 Memory Allocator Sample";
const int SIZE_X = 1024;
const int SIZE_Y = 576; 
const bool FULLSCREEN = false;
const DXGI_FORMAT RENDER_TARGET_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
const DXGI_FORMAT DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D32_FLOAT;
const size_t FRAME_BUFFER_COUNT = 3; // number of buffers we want, 2 for double buffering, 3 for tripple buffering
static const D3D_FEATURE_LEVEL MY_D3D_FEATURE_LEVEL = D3D_FEATURE_LEVEL_12_0;

const bool ENABLE_DEBUG_LAYER = true;

HINSTANCE g_Instance;
HWND g_Wnd;

uint64_t g_TimeOffset;
uint64_t g_TimeValue;
float g_Time;
float g_TimeDelta;

CComPtr<ID3D12Device> g_Device; // direct3d g_Device
CComPtr<IDXGISwapChain3> g_SwapChain; // swapchain used to switch between render targets
CComPtr<ID3D12CommandQueue> g_CommandQueue; // container for command lists
CComPtr<ID3D12DescriptorHeap> g_RtvDescriptorHeap; // a descriptor heap to hold resources like the render targets
CComPtr<ID3D12Resource> g_RenderTargets[FRAME_BUFFER_COUNT]; // number of render targets equal to buffer count
CComPtr<ID3D12CommandAllocator> g_CommandAllocators[FRAME_BUFFER_COUNT]; // we want enough allocators for each buffer * number of threads (we only have one thread)
CComPtr<ID3D12GraphicsCommandList> g_CommandList; // a command list we can record commands into, then execute them to render the frame
CComPtr<ID3D12Fence> g_Fences[FRAME_BUFFER_COUNT];    // an object that is locked while our command list is being executed by the gpu. We need as many 
                                                      //as we have allocators (more if we want to know when the gpu is finished with an asset)
HANDLE g_FenceEvent; // a handle to an event when our g_Fences is unlocked by the gpu
UINT64 g_FenceValues[FRAME_BUFFER_COUNT]; // this value is incremented each frame. each g_Fences will have its own value
UINT g_FrameIndex; // current rtv we are on
UINT g_RtvDescriptorSize; // size of the rtv descriptor on the g_Device (all front and back buffers will be the same size)

CComPtr<ID3D12PipelineState> g_PipelineStateObject;
CComPtr<ID3D12RootSignature> g_RootSignature;
CComPtr<ID3D12Resource> g_VertexBuffer;
CComPtr<ID3D12Resource> g_IndexBuffer;
D3D12_VERTEX_BUFFER_VIEW g_VertexBufferView;
D3D12_INDEX_BUFFER_VIEW g_IndexBufferView;
CComPtr<ID3D12Resource> g_DepthStencilBuffer;
CComPtr<ID3D12DescriptorHeap> g_DepthStencilDescriptorHeap;

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT2 texCoord;
    XMFLOAT4 color;

    Vertex() { }
    Vertex(float x, float y, float z, float tx, float ty, float r, float g, float b, float a) :
        pos(x, y, z),
        texCoord(tx, ty),
        color(r, g, b, a)
    {
    }
};

struct ConstantBuffer
{
    XMFLOAT4 ColorMultiplier;
};

struct ConstantBufferPerObject
{
    XMFLOAT4X4 WorldViewProj;
};
size_t ConstantBufferPerObjectAlignedSize = AlignUp<size_t>(sizeof(ConstantBufferPerObject), 256);
CComPtr<ID3D12Resource> g_CbPertObjectUploadHeaps[FRAME_BUFFER_COUNT];
void* g_CbPerObjectAddress[FRAME_BUFFER_COUNT];
XMFLOAT4X4 cameraProjMat; // this will store our projection matrix
XMFLOAT4X4 cameraViewMat; // this will store our view matrix
XMFLOAT4X4 cube1WorldMat; // our first cubes world matrix (transformation matrix)
XMFLOAT4X4 cube1RotMat; // this will keep track of our rotation for the first cube
XMFLOAT4 cube1Position; // our first cubes position in space
XMFLOAT4X4 cube2WorldMat; // our first cubes world matrix (transformation matrix)
XMFLOAT4X4 cube2RotMat; // this will keep track of our rotation for the second cube
XMFLOAT4 cube2PositionOffset; // our second cube will rotate around the first cube, so this is the position offset from the first cube
uint32_t numCubeIndices; // the number of indices to draw the cube

CComPtr<ID3D12DescriptorHeap> g_MainDescriptorHeap[FRAME_BUFFER_COUNT];
CComPtr<ID3D12Resource> g_ConstantBufferUploadHeap[FRAME_BUFFER_COUNT];
void* g_ConstantBufferAddress[FRAME_BUFFER_COUNT];

CComPtr<IWICImagingFactory> g_WicImagingFactory;
CComPtr<ID3D12Resource> g_Texture;

DXGI_FORMAT GetDXGIFormatFromWICFormat(WICPixelFormatGUID& wicFormatGUID)
{
    if (wicFormatGUID == GUID_WICPixelFormat128bppRGBAFloat) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGBAHalf) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGBA) return DXGI_FORMAT_R16G16B16A16_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppRGBA) return DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppBGRA) return DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppBGR) return DXGI_FORMAT_B8G8R8X8_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppRGBA1010102XR) return DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;

    else if (wicFormatGUID == GUID_WICPixelFormat32bppRGBA1010102) return DXGI_FORMAT_R10G10B10A2_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppBGRA5551) return DXGI_FORMAT_B5G5R5A1_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppBGR565) return DXGI_FORMAT_B5G6R5_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppGrayFloat) return DXGI_FORMAT_R32_FLOAT;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppGrayHalf) return DXGI_FORMAT_R16_FLOAT;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppGray) return DXGI_FORMAT_R16_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat8bppGray) return DXGI_FORMAT_R8_UNORM;
    else if (wicFormatGUID == GUID_WICPixelFormat8bppAlpha) return DXGI_FORMAT_A8_UNORM;

    else return DXGI_FORMAT_UNKNOWN;
}

WICPixelFormatGUID GetConvertToWICFormat(WICPixelFormatGUID& wicFormatGUID)
{
    if (wicFormatGUID == GUID_WICPixelFormatBlackWhite) return GUID_WICPixelFormat8bppGray;
    else if (wicFormatGUID == GUID_WICPixelFormat1bppIndexed) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat2bppIndexed) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat4bppIndexed) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat8bppIndexed) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat2bppGray) return GUID_WICPixelFormat8bppGray;
    else if (wicFormatGUID == GUID_WICPixelFormat4bppGray) return GUID_WICPixelFormat8bppGray;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppGrayFixedPoint) return GUID_WICPixelFormat16bppGrayHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppGrayFixedPoint) return GUID_WICPixelFormat32bppGrayFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat16bppBGR555) return GUID_WICPixelFormat16bppBGRA5551;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppBGR101010) return GUID_WICPixelFormat32bppRGBA1010102;
    else if (wicFormatGUID == GUID_WICPixelFormat24bppBGR) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat24bppRGB) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppPBGRA) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppPRGBA) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat48bppRGB) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat48bppBGR) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppBGRA) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppPRGBA) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppPBGRA) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat48bppRGBFixedPoint) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat48bppBGRFixedPoint) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGBAFixedPoint) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppBGRAFixedPoint) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGBFixedPoint) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGBHalf) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat48bppRGBHalf) return GUID_WICPixelFormat64bppRGBAHalf;
    else if (wicFormatGUID == GUID_WICPixelFormat128bppPRGBAFloat) return GUID_WICPixelFormat128bppRGBAFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat128bppRGBFloat) return GUID_WICPixelFormat128bppRGBAFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat128bppRGBAFixedPoint) return GUID_WICPixelFormat128bppRGBAFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat128bppRGBFixedPoint) return GUID_WICPixelFormat128bppRGBAFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppRGBE) return GUID_WICPixelFormat128bppRGBAFloat;
    else if (wicFormatGUID == GUID_WICPixelFormat32bppCMYK) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppCMYK) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat40bppCMYKAlpha) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat80bppCMYKAlpha) return GUID_WICPixelFormat64bppRGBA;

#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8) || defined(_WIN7_PLATFORM_UPDATE)
    else if (wicFormatGUID == GUID_WICPixelFormat32bppRGB) return GUID_WICPixelFormat32bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppRGB) return GUID_WICPixelFormat64bppRGBA;
    else if (wicFormatGUID == GUID_WICPixelFormat64bppPRGBAHalf) return GUID_WICPixelFormat64bppRGBAHalf;
#endif

    else return GUID_WICPixelFormatDontCare;
}

size_t GetDXGIFormatBitsPerPixel(DXGI_FORMAT& dxgiFormat)
{
    if (dxgiFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) return 128;
    else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) return 64;
    else if (dxgiFormat == DXGI_FORMAT_R16G16B16A16_UNORM) return 64;
    else if (dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM) return 32;
    else if (dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM) return 32;
    else if (dxgiFormat == DXGI_FORMAT_B8G8R8X8_UNORM) return 32;
    else if (dxgiFormat == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) return 32;

    else if (dxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM) return 32;
    else if (dxgiFormat == DXGI_FORMAT_B5G5R5A1_UNORM) return 16;
    else if (dxgiFormat == DXGI_FORMAT_B5G6R5_UNORM) return 16;
    else if (dxgiFormat == DXGI_FORMAT_R32_FLOAT) return 32;
    else if (dxgiFormat == DXGI_FORMAT_R16_FLOAT) return 16;
    else if (dxgiFormat == DXGI_FORMAT_R16_UNORM) return 16;
    else if (dxgiFormat == DXGI_FORMAT_R8_UNORM) return 8;
    else if (dxgiFormat == DXGI_FORMAT_A8_UNORM) return 8;

    assert(0);
    return 0;
}

size_t LoadImageDataFromFile(
    const wchar_t* filePath,
    std::vector<char>& imageData,
    D3D12_RESOURCE_DESC& resourceDescription,
    size_t &bytesPerRow)
{
    CComPtr<IWICBitmapDecoder> decoder;
    CHECK_HR( g_WicImagingFactory->CreateDecoderFromFilename(
        filePath,
        NULL,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder) );

    CComPtr<IWICBitmapFrameDecode> frame;
    CHECK_HR( decoder->GetFrame(0, &frame) );

    WICPixelFormatGUID pixelFormat;
    CHECK_HR( frame->GetPixelFormat(&pixelFormat) );

    UINT sizeX, sizeY;
    CHECK_HR( frame->GetSize(&sizeX, &sizeY) );

    DXGI_FORMAT dxgiFormat = GetDXGIFormatFromWICFormat(pixelFormat);

    IWICFormatConverter *wicConverter = NULL;
    if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
    {
        WICPixelFormatGUID convertToPixelFormat = GetConvertToWICFormat(pixelFormat);
        assert(convertToPixelFormat != GUID_WICPixelFormatDontCare);
        dxgiFormat = GetDXGIFormatFromWICFormat(convertToPixelFormat);

        CHECK_HR( g_WicImagingFactory->CreateFormatConverter(&wicConverter) );

        BOOL canConvert = FALSE;
        CHECK_HR( wicConverter->CanConvert(pixelFormat, convertToPixelFormat, &canConvert) );
        assert(canConvert);

        CHECK_HR( wicConverter->Initialize(frame, convertToPixelFormat, WICBitmapDitherTypeErrorDiffusion, 0, 0, WICBitmapPaletteTypeCustom) );
    }

    size_t bitsPerPixel = GetDXGIFormatBitsPerPixel(dxgiFormat);
    bytesPerRow = (sizeX * bitsPerPixel) / 8;
    size_t imageSize = bytesPerRow * sizeY;

    imageData.resize(imageSize);

    if (wicConverter)
    {
        CHECK_HR( wicConverter->CopyPixels(0, (UINT)bytesPerRow, (UINT)imageSize, (BYTE*)imageData.data()) );
    }
    else
    {
        CHECK_HR( frame->CopyPixels(0, (UINT)bytesPerRow, (UINT)imageSize, (BYTE*)imageData.data()) );
    }

    resourceDescription = {};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDescription.Alignment = 0;
    resourceDescription.Width = sizeX;
    resourceDescription.Height = sizeY;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;
    resourceDescription.Format = dxgiFormat;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.SampleDesc.Quality = 0;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

    return imageSize;
}

ID3DBlob* CompileShader(
    const wchar_t* filePath,
    const char* entryPoint,
    const char* target)
{
    ID3DBlob* shaderPtr = nullptr;
    ID3DBlob* errorBuffPtr = nullptr;
    wprintf(L"Compiling shader \"%s\"\n", filePath);
    HRESULT hr = D3DCompileFromFile(filePath,
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
        0,
        &shaderPtr,
        &errorBuffPtr);
    CComPtr<ID3DBlob> errorBuff{errorBuffPtr};
    if(errorBuff && errorBuffPtr->GetBufferSize())
        printf("%s\n", (char*)errorBuff->GetBufferPointer());
    CHECK_HR(hr);
    return shaderPtr;
}

void WaitForFrame(size_t frameIndex) // wait until gpu is finished with command list
{
    // if the current g_Fences value is still less than "g_FenceValues", then we know the GPU has not finished executing
    // the command queue since it has not reached the "g_CommandQueue->Signal(g_Fences, g_FenceValues)" command
    if (g_Fences[frameIndex]->GetCompletedValue() < g_FenceValues[frameIndex])
    {
        // we have the g_Fences create an event which is signaled once the g_Fences's current value is "g_FenceValues"
        CHECK_HR( g_Fences[frameIndex]->SetEventOnCompletion(g_FenceValues[frameIndex], g_FenceEvent) );

        // We will wait until the g_Fences has triggered the event that it's current value has reached "g_FenceValues". once it's value
        // has reached "g_FenceValues", we know the command queue has finished executing
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
}

void WaitGPUIdle(size_t frameIndex)
{
    g_FenceValues[frameIndex]++;
    CHECK_HR( g_CommandQueue->Signal(g_Fences[frameIndex], g_FenceValues[frameIndex]) );
    WaitForFrame(frameIndex);
}

void InitD3D() // initializes direct3d 12
{
    CHECK_HR( CoCreateInstance(
        CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_WicImagingFactory)) );

    IDXGIFactory4* dxgiFactory;
    CHECK_HR( CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) );

    IDXGIAdapter1* adapter = nullptr; // adapters are the graphics card (this includes the embedded graphics on the motherboard)

    int adapterIndex = 0; // we'll start looking for directx 12  compatible graphics devices starting at index 0

    bool adapterFound = false; // set this to true when a good one was found

                               // find first hardware gpu that supports d3d 12
    while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            HRESULT hr = D3D12CreateDevice(adapter, MY_D3D_FEATURE_LEVEL, _uuidof(ID3D12Device), nullptr);
            if (SUCCEEDED(hr))
            {
                adapterFound = true;
                break;
            }
        }
        adapter->Release();
        adapterIndex++;
    }
    assert(adapterFound);

    // Must be done before D3D12 device is created.
    if(ENABLE_DEBUG_LAYER)
    {
        CComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }

    // Create the g_Device
    ID3D12Device* device = nullptr;
    CHECK_HR( D3D12CreateDevice(
        adapter,
        MY_D3D_FEATURE_LEVEL,
        IID_PPV_ARGS(&device)) );
    g_Device.Attach(device);

    // -- Create the Command Queue -- //

    D3D12_COMMAND_QUEUE_DESC cqDesc = {}; // we will be using all the default values

    ID3D12CommandQueue* commandQueue = nullptr;
    CHECK_HR( g_Device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&commandQueue)) ); // create the command queue
    g_CommandQueue.Attach(commandQueue);

    // -- Create the Swap Chain (double/tripple buffering) -- //

    DXGI_MODE_DESC backBufferDesc = {}; // this is to describe our display mode
    backBufferDesc.Width = SIZE_X; // buffer width
    backBufferDesc.Height = SIZE_Y; // buffer height
    backBufferDesc.Format = RENDER_TARGET_FORMAT; // format of the buffer (rgba 32 bits, 8 bits for each chanel)

                                                  // describe our multi-sampling. We are not multi-sampling, so we set the count to 1 (we need at least one sample of course)
    DXGI_SAMPLE_DESC sampleDesc = {};
    sampleDesc.Count = 1; // multisample count (no multisampling, so we just put 1, since we still need 1 sample)

                          // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_BUFFER_COUNT; // number of buffers we have
    swapChainDesc.BufferDesc = backBufferDesc; // our back buffer description
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // this says the pipeline will render to this swap chain
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // dxgi will discard the buffer (data) after we call present
    swapChainDesc.OutputWindow = g_Wnd; // handle to our window
    swapChainDesc.SampleDesc = sampleDesc; // our multi-sampling description
    swapChainDesc.Windowed = !FULLSCREEN; // set to true, then if in fullscreen must call SetFullScreenState with true for full screen to get uncapped fps

    IDXGISwapChain* tempSwapChain;

    CHECK_HR( dxgiFactory->CreateSwapChain(
        g_CommandQueue, // the queue will be flushed once the swap chain is created
        &swapChainDesc, // give it the swap chain description we created above
        &tempSwapChain // store the created swap chain in a temp IDXGISwapChain interface
    ) );

    g_SwapChain.Attach(static_cast<IDXGISwapChain3*>(tempSwapChain));

    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex(); 

    // -- Create the Back Buffers (render target views) Descriptor Heap -- //

    // describe an rtv descriptor heap and create
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_BUFFER_COUNT; // number of descriptors for this heap.
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; // this heap is a render target view heap

                                                       // This heap will not be directly referenced by the shaders (not shader visible), as this will store the output from the pipeline
                                                       // otherwise we would set the heap's flag to D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* rtvDescriptorHeap = nullptr;
    CHECK_HR( g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvDescriptorHeap)) );
    g_RtvDescriptorHeap.Attach(rtvDescriptorHeap);

    // get the size of a descriptor in this heap (this is a rtv heap, so only rtv descriptors should be stored in it.
    // descriptor sizes may vary from g_Device to g_Device, which is why there is no set size and we must ask the 
    // g_Device to give us the size. we will use this size to increment a descriptor handle offset
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // get a handle to the first descriptor in the descriptor heap. a handle is basically a pointer,
    // but we cannot literally use it like a c++ pointer.
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // Create a RTV for each buffer (double buffering is two buffers, tripple buffering is 3).
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        // first we get the n'th buffer in the swap chain and store it in the n'th
        // position of our ID3D12Resource array
        ID3D12Resource* res = nullptr;
        CHECK_HR( g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&res)) );
        g_RenderTargets[i].Attach(res);

        // the we "create" a render target view which binds the swap chain buffer (ID3D12Resource[n]) to the rtv handle
        g_Device->CreateRenderTargetView(g_RenderTargets[i], nullptr, rtvHandle);

        // we increment the rtv handle by the rtv descriptor size we got above
        rtvHandle.Offset(1, g_RtvDescriptorSize);
    }

    // -- Create the Command Allocators -- //

    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        ID3D12CommandAllocator* commandAllocator = nullptr;
        CHECK_HR( g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) );
        g_CommandAllocators[i].Attach(commandAllocator);
    }

    // create the command list with the first allocator
    CHECK_HR( g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[0], NULL, IID_PPV_ARGS(&g_CommandList)) );

    // command lists are created in the recording state. our main loop will set it up for recording again so close it now
    g_CommandList->Close();

    // create a depth stencil descriptor heap so we can get a pointer to the depth stencil buffer
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK_HR( g_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_DepthStencilDescriptorHeap)) );

    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    CHECK_HR( g_Device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Tex2D(DEPTH_STENCIL_FORMAT, SIZE_X, SIZE_Y, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,
        IID_PPV_ARGS(&g_DepthStencilBuffer)
    ) );
    CHECK_HR( g_DepthStencilBuffer->SetName(L"Depth/Stencil Resource Heap") );

    D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
    depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
    g_Device->CreateDepthStencilView(g_DepthStencilBuffer, &depthStencilDesc, g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // -- Create a Fence & Fence Event -- //

    // create the fences
    for (int i = 0; i < FRAME_BUFFER_COUNT; i++)
    {
        ID3D12Fence* fence = nullptr;
        CHECK_HR( g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) );
        g_Fences[i].Attach(fence);
        g_FenceValues[i] = 0; // set the initial g_Fences value to 0
    }

    // create a handle to a g_Fences event
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    assert(g_FenceEvent);

    // create a descriptor range (descriptor table) and fill it out
    // this is a range of descriptors inside a descriptor heap
    D3D12_DESCRIPTOR_RANGE cbDescriptorRange; // only one range right now
    cbDescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; // this is a range of constant buffer views (descriptors)
    cbDescriptorRange.NumDescriptors = 1; // we only have one constant buffer, so the range is only 1
    cbDescriptorRange.BaseShaderRegister = 0; // start index of the shader registers in the range
    cbDescriptorRange.RegisterSpace = 0; // space 0. can usually be zero
    cbDescriptorRange.OffsetInDescriptorsFromTableStart = 0; // this appends the range to the end of the root signature descriptor tables

    D3D12_DESCRIPTOR_RANGE textureDescRange;
    textureDescRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureDescRange.NumDescriptors = 1;
    textureDescRange.BaseShaderRegister = 0;
    textureDescRange.RegisterSpace = 0;
    textureDescRange.OffsetInDescriptorsFromTableStart = 1;

    // create a root parameter and fill it out
    D3D12_ROOT_PARAMETER  rootParameters[3]; // only one parameter right now

    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // this is a descriptor table
    rootParameters[0].DescriptorTable = CD3DX12_ROOT_DESCRIPTOR_TABLE(1, &cbDescriptorRange);
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX; // our pixel shader will be the only shader accessing this parameter for now

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].Descriptor = CD3DX12_ROOT_DESCRIPTOR(1);
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable = CD3DX12_ROOT_DESCRIPTOR_TABLE(1, &textureDescRange);
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // create root signature

    // create a static sampler
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(
        _countof(rootParameters), rootParameters,
        1,
        &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    CComPtr<ID3DBlob> signatureBlob;
    ID3DBlob* signatureBlobPtr;
    CHECK_HR( D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlobPtr, nullptr) );
    signatureBlob.Attach(signatureBlobPtr);

    ID3D12RootSignature* rootSignature = nullptr;
    CHECK_HR( device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)) );
    g_RootSignature.Attach(rootSignature);

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        CHECK_HR( g_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_MainDescriptorHeap[i])) );
    }

    // # CONSTANT BUFFER

    for (int i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        CHECK_HR( g_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(1024 * 64),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&g_ConstantBufferUploadHeap[i])) );
        g_ConstantBufferUploadHeap[i]->SetName(L"Constant Buffer Upload Resource Heap");

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = g_ConstantBufferUploadHeap[i]->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = AlignUp<UINT>(sizeof(ConstantBuffer), 256);
        g_Device->CreateConstantBufferView(&cbvDesc, g_MainDescriptorHeap[i]->GetCPUDescriptorHandleForHeapStart());

        CD3DX12_RANGE readRange(0, 0);
        CHECK_HR( g_ConstantBufferUploadHeap[i]->Map(0, &readRange, &g_ConstantBufferAddress[i]) );
    }

    // create vertex and pixel shaders

    // when debugging, we can compile the shader files at runtime.
    // but for release versions, we can compile the hlsl shaders
    // with fxc.exe to create .cso files, which contain the shader
    // bytecode. We can load the .cso files at runtime to get the
    // shader bytecode, which of course is faster than compiling
    // them at runtime

    // compile vertex shader
    CComPtr<ID3DBlob> vertexShader{CompileShader(L"Shaders/VS.hlsl", "main", "vs_5_0")};

    // compile pixel shader
    CComPtr<ID3DBlob> pixelShader{CompileShader(L"Shaders/PS.hlsl", "main", "ps_5_0")};

    // create input layout

    // The input layout is used by the Input Assembler so that it knows
    // how to read the vertex data bound to it.

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // create a pipeline state object (PSO)

    // In a real application, you will have many pso's. for each different shader
    // or different combinations of shaders, different blend states or different rasterizer states,
    // different topology types (point, line, triangle, patch), or a different number
    // of render targets you will need a pso

    // VS is the only required shader for a pso. You might be wondering when a case would be where
    // you only set the VS. It's possible that you have a pso that only outputs data with the stream
    // output, and not on a render target, which means you would not need anything after the stream
    // output.

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {}; // a structure to define a pso
    psoDesc.InputLayout.NumElements = _countof(inputLayout);
    psoDesc.InputLayout.pInputElementDescs = inputLayout;
    psoDesc.pRootSignature = g_RootSignature; // the root signature that describes the input data this pso needs
    psoDesc.VS.BytecodeLength = vertexShader->GetBufferSize();
    psoDesc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    psoDesc.PS.BytecodeLength = pixelShader->GetBufferSize();
    psoDesc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // type of topology we are drawing
    psoDesc.RTVFormats[0] = RENDER_TARGET_FORMAT; // format of the render target
    psoDesc.DSVFormat = DEPTH_STENCIL_FORMAT;
    psoDesc.SampleDesc = sampleDesc; // must be the same sample description as the swapchain and depth/stencil buffer
    psoDesc.SampleMask = 0xffffffff; // sample mask has to do with multi-sampling. 0xffffffff means point sampling is done
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); // a default rasterizer state.
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // a default blent state.
    psoDesc.NumRenderTargets = 1; // we are only binding one render target
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    // create the pso
    ID3D12PipelineState* pipelineStateObject;
    CHECK_HR( device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineStateObject)) );
    g_PipelineStateObject.Attach(pipelineStateObject);

    // Create vertex buffer

    // a triangle
    Vertex vList[] = {
        // front face
        { -0.5f,  0.5f, -0.5f, 0.f, 0.f, 1.0f, 0.0f, 0.0f, 1.0f },
    {  0.5f, -0.5f, -0.5f, 1.f, 1.f, 1.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f, -0.5f, -0.5f, 0.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f,  0.5f, -0.5f, 1.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },

    // right side face
    {  0.5f, -0.5f, -0.5f, 0.f, 1.f, 1.0f, 0.0f, 0.0f, 1.0f },
    {  0.5f,  0.5f,  0.5f, 1.f, 0.f, 1.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f, -0.5f,  0.5f, 1.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f,  0.5f, -0.5f, 0.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },

    // left side face
    { -0.5f,  0.5f,  0.5f, 0.f, 0.f, 1.0f, 0.0f, 0.0f, 1.0f },
    { -0.5f, -0.5f, -0.5f, 1.f, 1.f, 1.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f, -0.5f,  0.5f, 0.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f,  0.5f, -0.5f, 1.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },

    // back face
    {  0.5f,  0.5f,  0.5f, 0.f, 0.f, 1.0f, 0.0f, 0.0f, 1.0f },
    { -0.5f, -0.5f,  0.5f, 1.f, 1.f, 1.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f, -0.5f,  0.5f, 0.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f,  0.5f,  0.5f, 1.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },

    // top face
    { -0.5f,  0.5f, -0.5f, 0.f, 0.f, 1.0f, 0.0f, 0.0f, 1.0f },
    {  0.5f,  0.5f,  0.5f, 1.f, 1.f, 1.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f,  0.5f, -0.5f, 0.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f,  0.5f,  0.5f, 1.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },

    // bottom face
    {  0.5f, -0.5f,  0.5f, 0.f, 0.f, 1.0f, 0.0f, 0.0f, 1.0f },
    { -0.5f, -0.5f, -0.5f, 1.f, 1.f, 1.0f, 0.0f, 1.0f, 1.0f },
    {  0.5f, -0.5f, -0.5f, 0.f, 1.f, 0.0f, 0.0f, 1.0f, 1.0f },
    { -0.5f, -0.5f,  0.5f, 1.f, 0.f, 0.0f, 1.0f, 0.0f, 1.0f },
    };
    const uint32_t vBufferSize = sizeof(vList);

    // create default heap
    // default heap is memory on the GPU. Only the GPU has access to this memory
    // To get data into this heap, we will have to upload the data using
    // an upload heap
    ID3D12Resource* vertexBufferPtr;
    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // a default heap
        D3D12_HEAP_FLAG_NONE, // no flags
        &CD3DX12_RESOURCE_DESC::Buffer(vBufferSize), // resource description for a buffer
        D3D12_RESOURCE_STATE_COPY_DEST, // we will start this heap in the copy destination state since we will copy data
                                        // from the upload heap to this heap
        nullptr, // optimized clear value must be null for this type of resource. used for render targets and depth/stencil buffers
        IID_PPV_ARGS(&vertexBufferPtr)) );
    g_VertexBuffer.Attach(vertexBufferPtr);

    // we can give resource heaps a name so when we debug with the graphics debugger we know what resource we are looking at
    g_VertexBuffer->SetName(L"Vertex Buffer Resource Heap");

    // create upload heap
    // upload heaps are used to upload data to the GPU. CPU can write to it, GPU can read from it
    // We will upload the vertex buffer using this heap to the default heap
    CComPtr<ID3D12Resource> vBufferUploadHeap;
    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // upload heap
        D3D12_HEAP_FLAG_NONE, // no flags
        &CD3DX12_RESOURCE_DESC::Buffer(vBufferSize), // resource description for a buffer
        D3D12_RESOURCE_STATE_GENERIC_READ, // GPU will read from this buffer and copy its contents to the default heap
        nullptr,
        IID_PPV_ARGS(&vBufferUploadHeap)) );
    vBufferUploadHeap->SetName(L"Vertex Buffer Upload Resource Heap");

    // store vertex buffer in upload heap
    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = reinterpret_cast<BYTE*>(vList); // pointer to our vertex array
    vertexData.RowPitch = vBufferSize; // size of all our triangle vertex data
    vertexData.SlicePitch = vBufferSize; // also the size of our triangle vertex data

    CHECK_HR( g_CommandList->Reset(g_CommandAllocators[g_FrameIndex], NULL) );

    // we are now creating a command with the command list to copy the data from
    // the upload heap to the default heap
    UINT64 r = UpdateSubresources(g_CommandList, g_VertexBuffer, vBufferUploadHeap, 0, 0, 1, &vertexData);
    assert(r);

    // transition the vertex buffer data from copy destination state to vertex buffer state
    g_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_VertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

    // Create index buffer

    // a quad (2 triangles)
    uint16_t iList[] = {
        // ffront face
        0, 1, 2, // first triangle
        0, 3, 1, // second triangle

                 // left face
                 4, 5, 6, // first triangle
                 4, 7, 5, // second triangle

                          // right face
                          8, 9, 10, // first triangle
                          8, 11, 9, // second triangle

                                    // back face
                                    12, 13, 14, // first triangle
                                    12, 15, 13, // second triangle

                                                // top face
                                                16, 17, 18, // first triangle
                                                16, 19, 17, // second triangle

                                                            // bottom face
                                                            20, 21, 22, // first triangle
                                                            20, 23, 21, // second triangle
    };

    numCubeIndices = (uint32_t)_countof(iList);

    size_t iBufferSize = sizeof(iList);

    // create default heap to hold index buffer
    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // a default heap
        D3D12_HEAP_FLAG_NONE, // no flags
        &CD3DX12_RESOURCE_DESC::Buffer(iBufferSize), // resource description for a buffer
        D3D12_RESOURCE_STATE_COPY_DEST, // start in the copy destination state
        nullptr, // optimized clear value must be null for this type of resource
        IID_PPV_ARGS(&g_IndexBuffer)) );

    // we can give resource heaps a name so when we debug with the graphics debugger we know what resource we are looking at
    g_IndexBuffer->SetName(L"Index Buffer Resource Heap");

    // create upload heap to upload index buffer
    CComPtr<ID3D12Resource> iBufferUploadHeap;
    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // upload heap
        D3D12_HEAP_FLAG_NONE, // no flags
        &CD3DX12_RESOURCE_DESC::Buffer(vBufferSize), // resource description for a buffer
        D3D12_RESOURCE_STATE_GENERIC_READ, // GPU will read from this buffer and copy its contents to the default heap
        nullptr,
        IID_PPV_ARGS(&iBufferUploadHeap)) );
    CHECK_HR( vBufferUploadHeap->SetName(L"Index Buffer Upload Resource Heap") );

    // store vertex buffer in upload heap
    D3D12_SUBRESOURCE_DATA indexData = {};
    indexData.pData = iList; // pointer to our index array
    indexData.RowPitch = iBufferSize; // size of all our index buffer
    indexData.SlicePitch = iBufferSize; // also the size of our index buffer

                                        // we are now creating a command with the command list to copy the data from
                                        // the upload heap to the default heap
    r = UpdateSubresources(g_CommandList, g_IndexBuffer, iBufferUploadHeap, 0, 0, 1, &indexData);
    assert(r);

    // transition the vertex buffer data from copy destination state to vertex buffer state
    g_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        g_IndexBuffer,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDEX_BUFFER));

    // create a vertex buffer view for the triangle. We get the GPU memory address to the vertex pointer using the GetGPUVirtualAddress() method
    g_VertexBufferView.BufferLocation = g_VertexBuffer->GetGPUVirtualAddress();
    g_VertexBufferView.StrideInBytes = sizeof(Vertex);
    g_VertexBufferView.SizeInBytes = vBufferSize;

    // create a index buffer view for the triangle. We get the GPU memory address to the vertex pointer using the GetGPUVirtualAddress() method
    g_IndexBufferView.BufferLocation = g_IndexBuffer->GetGPUVirtualAddress();
    g_IndexBufferView.Format = DXGI_FORMAT_R16_UINT;
    g_IndexBufferView.SizeInBytes = (UINT)iBufferSize;

    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        // create resource for cube 1
        CHECK_HR( g_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // this heap will be used to upload the constant buffer data
            D3D12_HEAP_FLAG_NONE, // no flags
            &CD3DX12_RESOURCE_DESC::Buffer(1024 * 64), // size of the resource heap. Must be a multiple of 64KB for single-textures and constant buffers
            D3D12_RESOURCE_STATE_GENERIC_READ, // will be data that is read from so we keep it in the generic read state
            nullptr, // we do not have use an optimized clear value for constant buffers
            IID_PPV_ARGS(&g_CbPertObjectUploadHeaps[i])) );
        g_CbPertObjectUploadHeaps[i]->SetName(L"Constant Buffer Upload Resource Heap");

        CD3DX12_RANGE readRange(0, 0);    // We do not intend to read from this resource on the CPU. (so end is less than or equal to begin)
                                          // map the resource heap to get a gpu virtual address to the beginning of the heap
        CHECK_HR( g_CbPertObjectUploadHeaps[i]->Map(0, &readRange, &g_CbPerObjectAddress[i]) );
    }

    // build projection and view matrix
    XMMATRIX tmpMat = XMMatrixPerspectiveFovLH(45.0f*(3.14f/180.0f), (float)SIZE_X / (float)SIZE_Y, 0.1f, 1000.0f);
    XMStoreFloat4x4(&cameraProjMat, tmpMat);

    // set starting camera state
    XMFLOAT4 cameraPosition; // this is our cameras position vector
    XMFLOAT4 cameraTarget; // a vector describing the point in space our camera is looking at
    XMFLOAT4 cameraUp; // the worlds up vector
    cameraPosition = XMFLOAT4(0.0f, 2.0f, -4.0f, 0.0f);
    cameraTarget = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    cameraUp = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);

    // build view matrix
    XMVECTOR cPos = XMLoadFloat4(&cameraPosition);
    XMVECTOR cTarg = XMLoadFloat4(&cameraTarget);
    XMVECTOR cUp = XMLoadFloat4(&cameraUp);
    tmpMat = XMMatrixLookAtLH(cPos, cTarg, cUp);
    XMStoreFloat4x4(&cameraViewMat, tmpMat);

    // set starting cubes position
    // first cube
    cube1Position = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f); // set cube 1's position
    XMVECTOR posVec = XMLoadFloat4(&cube1Position); // create xmvector for cube1's position

    tmpMat = XMMatrixTranslationFromVector(posVec); // create translation matrix from cube1's position vector
    XMStoreFloat4x4(&cube1RotMat, XMMatrixIdentity()); // initialize cube1's rotation matrix to identity matrix
    XMStoreFloat4x4(&cube1WorldMat, tmpMat); // store cube1's world matrix

                                             // second cube
    cube2PositionOffset = XMFLOAT4(1.5f, 0.0f, 0.0f, 0.0f);
    posVec = XMLoadFloat4(&cube2PositionOffset) + XMLoadFloat4(&cube1Position); // create xmvector for cube2's position
                                                                                // we are rotating around cube1 here, so add cube2's position to cube1

    tmpMat = XMMatrixTranslationFromVector(posVec); // create translation matrix from cube2's position offset vector
    XMStoreFloat4x4(&cube2RotMat, XMMatrixIdentity()); // initialize cube2's rotation matrix to identity matrix
    XMStoreFloat4x4(&cube2WorldMat, tmpMat); // store cube2's world matrix

                                             // # TEXTURE

    D3D12_RESOURCE_DESC textureDesc;
    size_t imageBytesPerRow;
    std::vector<char> imageData;
    size_t imageSize = LoadImageDataFromFile(L"TestCard 256x256.png", imageData, textureDesc, imageBytesPerRow);
    assert(imageSize > 0);

    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, // pOptimizedClearValue
        IID_PPV_ARGS(&g_Texture)) );
    g_Texture->SetName(L"g_Texture");

    UINT64 textureUploadBufferSize;
    device->GetCopyableFootprints(
        &textureDesc,
        0, // FirstSubresource
        1, // NumSubresources
        0, // BaseOffset
        nullptr, // pLayouts
        nullptr, // pNumRows
        nullptr, // pRowSizeInBytes
        &textureUploadBufferSize); // pTotalBytes

    CComPtr<ID3D12Resource> textureUpload;
    CHECK_HR( device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(textureUploadBufferSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, /// pOptimizedClearValue
        IID_PPV_ARGS(&textureUpload)) );
    textureUpload->SetName(L"textureUpload");

    D3D12_SUBRESOURCE_DATA textureSubresourceData = {};
    textureSubresourceData.pData = imageData.data();
    textureSubresourceData.RowPitch = imageBytesPerRow;
    textureSubresourceData.SlicePitch = imageBytesPerRow * textureDesc.Height;

    UpdateSubresources(g_CommandList, g_Texture, textureUpload, 0, 0, 1, &textureSubresourceData);

    g_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        g_Texture,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        g_Device->CreateShaderResourceView(g_Texture, &srvDesc, CD3DX12_CPU_DESCRIPTOR_HANDLE(
            g_MainDescriptorHeap[i]->GetCPUDescriptorHandleForHeapStart(),
            1,
            g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    }

    // # END OF INITIAL COMMAND LIST

    // Now we execute the command list to upload the initial assets (triangle data)
    g_CommandList->Close();
    ID3D12CommandList* ppCommandLists[] = { g_CommandList };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // increment the fence value now, otherwise the buffer might not be uploaded by the time we start drawing
    WaitGPUIdle(g_FrameIndex);
}

void Update()
{
    {
        float f_Linear = sin(g_Time * (XM_PI * 2.f)) * 0.5f + 0.5f;
        float f_sRGB = pow(f_Linear, 1.f / 2.2f);

        ConstantBuffer cb;
        cb.ColorMultiplier = XMFLOAT4(f_sRGB, f_sRGB, f_sRGB, 1.f);
        memcpy(g_ConstantBufferAddress[g_FrameIndex], &cb, sizeof(cb));
    }

    {
        // update app logic, such as moving the camera or figuring out what objects are in view
        ConstantBufferPerObject cb;

        // create rotation matrices
        XMMATRIX rotXMat = XMMatrixRotationX(0.0001f);
        XMMATRIX rotYMat = XMMatrixRotationY(0.0002f);
        XMMATRIX rotZMat = XMMatrixRotationZ(0.0003f);

        // add rotation to cube1's rotation matrix and store it
        XMMATRIX rotMat = XMLoadFloat4x4(&cube1RotMat) * rotXMat * rotYMat * rotZMat;
        XMStoreFloat4x4(&cube1RotMat, rotMat);

        // create translation matrix for cube 1 from cube 1's position vector
        XMMATRIX translationMat = XMMatrixTranslationFromVector(XMLoadFloat4(&cube1Position));

        // create cube1's world matrix by first rotating the cube, then positioning the rotated cube
        XMMATRIX worldMat = rotMat * translationMat;

        // store cube1's world matrix
        XMStoreFloat4x4(&cube1WorldMat, worldMat);

        // update constant buffer for cube1
        // create the wvp matrix and store in constant buffer
        XMMATRIX viewMat = XMLoadFloat4x4(&cameraViewMat); // load view matrix
        XMMATRIX projMat = XMLoadFloat4x4(&cameraProjMat); // load projection matrix
        XMMATRIX wvpMat = XMLoadFloat4x4(&cube1WorldMat) * viewMat * projMat; // create wvp matrix
        XMMATRIX transposed = XMMatrixTranspose(wvpMat); // must transpose wvp matrix for the gpu
        XMStoreFloat4x4(&cb.WorldViewProj, transposed); // store transposed wvp matrix in constant buffer

                                                        // copy our ConstantBuffer instance to the mapped constant buffer resource
        memcpy(g_CbPerObjectAddress[g_FrameIndex], &cb, sizeof(cb));

        // now do cube2's world matrix
        // create rotation matrices for cube2
        rotXMat = XMMatrixRotationX(0.0003f);
        rotYMat = XMMatrixRotationY(0.0002f);
        rotZMat = XMMatrixRotationZ(0.0001f);

        // add rotation to cube2's rotation matrix and store it
        rotMat = rotZMat * (XMLoadFloat4x4(&cube2RotMat) * (rotXMat * rotYMat));
        XMStoreFloat4x4(&cube2RotMat, rotMat);

        // create translation matrix for cube 2 to offset it from cube 1 (its position relative to cube1
        XMMATRIX translationOffsetMat = XMMatrixTranslationFromVector(XMLoadFloat4(&cube2PositionOffset));

        // we want cube 2 to be half the size of cube 1, so we scale it by .5 in all dimensions
        XMMATRIX scaleMat = XMMatrixScaling(0.5f, 0.5f, 0.5f);

        // reuse worldMat. 
        // first we scale cube2. scaling happens relative to point 0,0,0, so you will almost always want to scale first
        // then we translate it. 
        // then we rotate it. rotation always rotates around point 0,0,0
        // finally we move it to cube 1's position, which will cause it to rotate around cube 1
        worldMat = scaleMat * translationOffsetMat * rotMat * translationMat;

        wvpMat = XMLoadFloat4x4(&cube2WorldMat) * viewMat * projMat; // create wvp matrix
        transposed = XMMatrixTranspose(wvpMat); // must transpose wvp matrix for the gpu
        XMStoreFloat4x4(&cb.WorldViewProj, transposed); // store transposed wvp matrix in constant buffer

                                                        // copy our ConstantBuffer instance to the mapped constant buffer resource
        memcpy((char*)g_CbPerObjectAddress[g_FrameIndex] + ConstantBufferPerObjectAlignedSize, &cb, sizeof(cb));

        // store cube2's world matrix
        XMStoreFloat4x4(&cube2WorldMat, worldMat);
    }
}

void Render() // execute the command list
{
    // # Here was UpdatePipeline function.

    // swap the current rtv buffer index so we draw on the correct buffer
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    // We have to wait for the gpu to finish with the command allocator before we reset it
    WaitForFrame(g_FrameIndex);
    // increment g_FenceValues for next frame
    g_FenceValues[g_FrameIndex]++;

    // we can only reset an allocator once the gpu is done with it
    // resetting an allocator frees the memory that the command list was stored in
    CHECK_HR( g_CommandAllocators[g_FrameIndex]->Reset() );

    // reset the command list. by resetting the command list we are putting it into
    // a recording state so we can start recording commands into the command allocator.
    // the command allocator that we reference here may have multiple command lists
    // associated with it, but only one can be recording at any time. Make sure
    // that any other command lists associated to this command allocator are in
    // the closed state (not recording).
    // Here you will pass an initial pipeline state object as the second parameter,
    // but in this tutorial we are only clearing the rtv, and do not actually need
    // anything but an initial default pipeline, which is what we get by setting
    // the second parameter to NULL
    CHECK_HR( g_CommandList->Reset(g_CommandAllocators[g_FrameIndex], NULL) );

    // here we start recording commands into the g_CommandList (which all the commands will be stored in the g_CommandAllocators)

    // transition the "g_FrameIndex" render target from the present state to the render target state so the command list draws to it starting from here
    g_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_RenderTargets[g_FrameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // here we again get the handle to our current render target view so we can set it as the render target in the output merger stage of the pipeline
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), g_FrameIndex, g_RtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // set the render target for the output merger stage (the output of the pipeline)
    g_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    g_CommandList->ClearDepthStencilView(g_DepthStencilDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Clear the render target by using the ClearRenderTargetView command
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    g_CommandList->SetPipelineState(g_PipelineStateObject);

    g_CommandList->SetGraphicsRootSignature(g_RootSignature);

    ID3D12DescriptorHeap* descriptorHeaps[] = { g_MainDescriptorHeap[g_FrameIndex] };
    g_CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    g_CommandList->SetGraphicsRootDescriptorTable(0, g_MainDescriptorHeap[g_FrameIndex]->GetGPUDescriptorHandleForHeapStart());
    g_CommandList->SetGraphicsRootDescriptorTable(2, g_MainDescriptorHeap[g_FrameIndex]->GetGPUDescriptorHandleForHeapStart());

    CD3DX12_VIEWPORT viewport{0.f, 0.f, (float)SIZE_X, (float)SIZE_Y, 0.f, 1.f};
    g_CommandList->RSSetViewports(1, &viewport); // set the viewports

    CD3DX12_RECT scissorRect{0, 0, SIZE_X, SIZE_Y};
    g_CommandList->RSSetScissorRects(1, &scissorRect); // set the scissor rects

    g_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // set the primitive topology
    g_CommandList->IASetVertexBuffers(0, 1, &g_VertexBufferView); // set the vertex buffer (using the vertex buffer view)
    g_CommandList->IASetIndexBuffer(&g_IndexBufferView);

    g_CommandList->SetGraphicsRootConstantBufferView(1,
        g_CbPertObjectUploadHeaps[g_FrameIndex]->GetGPUVirtualAddress());
    g_CommandList->DrawIndexedInstanced(numCubeIndices, 1, 0, 0, 0);

    g_CommandList->SetGraphicsRootConstantBufferView(1,
        g_CbPertObjectUploadHeaps[g_FrameIndex]->GetGPUVirtualAddress() + ConstantBufferPerObjectAlignedSize);
    g_CommandList->DrawIndexedInstanced(numCubeIndices, 1, 0, 0, 0);

    // transition the "g_FrameIndex" render target from the render target state to the present state. If the debug layer is enabled, you will receive a
    // warning if present is called on the render target when it's not in the present state
    g_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(g_RenderTargets[g_FrameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    CHECK_HR( g_CommandList->Close() );

    // ================

    // create an array of command lists (only one command list here)
    ID3D12CommandList* ppCommandLists[] = { g_CommandList };

    // execute the array of command lists
    g_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // this command goes in at the end of our command queue. we will know when our command queue 
    // has finished because the g_Fences value will be set to "g_FenceValues" from the GPU since the command
    // queue is being executed on the GPU
    CHECK_HR( g_CommandQueue->Signal(g_Fences[g_FrameIndex], g_FenceValues[g_FrameIndex]) );

    // present the current backbuffer
    CHECK_HR( g_SwapChain->Present(0, 0) );
}

void Cleanup() // release com ojects and clean up memory
{
    // wait for the gpu to finish all frames
    for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
    {
        WaitForFrame(i);
        CHECK_HR( g_CommandQueue->Wait(g_Fences[i], g_FenceValues[i]) );
    }

    // get swapchain out of full screen before exiting
    BOOL fs = false;
    CHECK_HR( g_SwapChain->GetFullscreenState(&fs, NULL) );
    if (fs)
        g_SwapChain->SetFullscreenState(false, NULL);

    WaitGPUIdle(0);

    g_Texture.Release();
    g_IndexBuffer.Release();
    g_VertexBuffer.Release();
    g_PipelineStateObject.Release();
    g_RootSignature.Release();

    CloseHandle(g_FenceEvent);
    g_CommandList.Release();
    g_CommandQueue.Release();

    for (size_t i = FRAME_BUFFER_COUNT; i--; )
    {
        g_CbPertObjectUploadHeaps[i].Release();
        g_MainDescriptorHeap[i].Release();
        g_ConstantBufferUploadHeap[i].Release();
    }

    g_DepthStencilDescriptorHeap.Release();
    g_DepthStencilBuffer.Release();
    g_RtvDescriptorHeap.Release();
    for (size_t i = FRAME_BUFFER_COUNT; i--; )
    {
        g_RenderTargets[i].Release();
        g_CommandAllocators[i].Release();
        g_Fences[i].Release();
    }
    g_Device.Release();
    g_SwapChain.Release();

    g_WicImagingFactory.Release();
}

static void OnKeyDown(WPARAM key)
{
    switch (key)
    {
    case VK_ESCAPE:
        PostMessage(g_Wnd, WM_CLOSE, 0, 0);
        break;
    }
}

static LRESULT WINAPI WndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
    case WM_CREATE:
        g_Wnd = wnd;
        InitD3D();
        g_TimeOffset = GetTickCount64();
        return 0;

    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    }

    return DefWindowProc(wnd, msg, wParam, lParam);
}

int main()
{
    g_Instance = (HINSTANCE)GetModuleHandle(NULL);

    CoInitialize(NULL);

    WNDCLASSEX wndClass;
    ZeroMemory(&wndClass, sizeof(wndClass));
    wndClass.cbSize = sizeof(wndClass);
    wndClass.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
    wndClass.hbrBackground = NULL;
    wndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndClass.hInstance = g_Instance;
    wndClass.lpfnWndProc = &WndProc;
    wndClass.lpszClassName = CLASS_NAME;

    ATOM classR = RegisterClassEx(&wndClass);
    assert(classR);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    DWORD exStyle = 0;

    RECT rect = { 0, 0, SIZE_X, SIZE_Y };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    g_Wnd = CreateWindowEx(
        exStyle,
        CLASS_NAME,
        WINDOW_TITLE,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL,
        NULL,
        g_Instance,
        0);
    assert(g_Wnd);

    MSG msg;
    for (;;)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            uint64_t newTimeValue = GetTickCount64() - g_TimeOffset;
            g_TimeDelta = (float)(newTimeValue - g_TimeValue) * 0.001f;
            g_Time = (float)newTimeValue * 0.001f;
            g_TimeValue = newTimeValue;

            Update();
            Render();
        }
    }
    return (int)msg.wParam;
} 
