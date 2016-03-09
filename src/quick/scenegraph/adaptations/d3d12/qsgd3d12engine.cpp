/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQuick module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qsgd3d12engine_p.h"
#include "qsgd3d12engine_p_p.h"
#include <QString>
#include <QColor>
#include <QtCore/private/qsimd_p.h>

QT_BEGIN_NAMESPACE

// NOTE: Avoid categorized logging. It is slow.

#define DECLARE_DEBUG_VAR(variable) \
    static bool debug_ ## variable() \
    { static bool value = qgetenv("QSG_RENDERER_DEBUG").contains(QT_STRINGIFY(variable)); return value; }

DECLARE_DEBUG_VAR(render)

static const int MAX_DRAW_CALLS_PER_LIST = 128;

static const int MAX_CACHED_ROOTSIG = 16;
static const int MAX_CACHED_PSO = 64;

static const int MAX_GPU_CBVSRVUAV_DESCRIPTORS = 1024;

static const int BUCKETS_PER_HEAP = 8; // must match freeMap
static const int DESCRIPTORS_PER_BUCKET = 32; // the bit map (freeMap) is quint32
static const int MAX_DESCRIPTORS_PER_HEAP = BUCKETS_PER_HEAP * DESCRIPTORS_PER_BUCKET;

D3D12_CPU_DESCRIPTOR_HANDLE QSGD3D12CPUDescriptorHeapManager::allocate(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = {};
    for (Heap &heap : m_heaps) {
        if (heap.type == type) {
            for (int bucket = 0; bucket < _countof(heap.freeMap); ++bucket)
                if (heap.freeMap[bucket]) {
                    unsigned long freePos = _bit_scan_forward(heap.freeMap[bucket]);
                    heap.freeMap[bucket] &= ~(1UL << freePos);
                    if (Q_UNLIKELY(debug_render()))
                        qDebug("descriptor handle type %x reserve in bucket %d index %d", type, bucket, freePos);
                    freePos += bucket * DESCRIPTORS_PER_BUCKET;
                    h = heap.start;
                    h.ptr += freePos * heap.handleSize;
                    return h;
                }
        }
    }

    Heap heap;
    heap.type = type;
    heap.handleSize = m_handleSizes[type];

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = MAX_DESCRIPTORS_PER_HEAP;
    heapDesc.Type = type;
    // The heaps created here are _never_ shader-visible.

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap.heap));
    if (FAILED(hr)) {
        qWarning("Failed to create heap with type 0x%x: %x", type, hr);
        return h;
    }

    heap.start = heap.heap->GetCPUDescriptorHandleForHeapStart();

    if (Q_UNLIKELY(debug_render()))
        qDebug("new descriptor heap, type %x, start %llu", type, heap.start.ptr);

    heap.freeMap[0] = 0xFFFFFFFE;
    for (int i = 1; i < _countof(heap.freeMap); ++i)
        heap.freeMap[i] = 0xFFFFFFFF;

    h = heap.start;

    m_heaps.append(heap);

    return h;
}

void QSGD3D12CPUDescriptorHeapManager::release(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    for (Heap &heap : m_heaps) {
        if (heap.type == type
                && handle.ptr >= heap.start.ptr
                && handle.ptr < heap.start.ptr + heap.handleSize * MAX_DESCRIPTORS_PER_HEAP) {
            unsigned long pos = (handle.ptr - heap.start.ptr) / heap.handleSize;
            const int bucket = pos / DESCRIPTORS_PER_BUCKET;
            const int indexInBucket = pos - bucket * DESCRIPTORS_PER_BUCKET;
            heap.freeMap[bucket] |= 1UL << indexInBucket;
            if (Q_UNLIKELY(debug_render()))
                qDebug("free descriptor handle type %x bucket %d index %d", type, bucket, indexInBucket);
            return;
        }
    }
    qWarning("QSGD3D12CPUDescriptorHeapManager: Attempted to release untracked descriptor handle %llu of type %d", handle.ptr, type);
}

void QSGD3D12CPUDescriptorHeapManager::initialize(ID3D12Device *device)
{
    m_device = device;

    for (int i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i)
        m_handleSizes[i] = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE(i));
}

void QSGD3D12CPUDescriptorHeapManager::releaseResources()
{
    for (Heap &heap : m_heaps)
        heap.heap = nullptr;

    m_heaps.clear();

    m_device = nullptr;
}

// One device per process, one everything else (engine) per window.
Q_GLOBAL_STATIC(QSGD3D12DeviceManager, deviceManager)

static void getHardwareAdapter(IDXGIFactory1 *factory, IDXGIAdapter1 **outAdapter)
{
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc;

    for (int adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        const QString name = QString::fromUtf16((char16_t *) desc.Description);
        qDebug("Adapter %d: '%s' (flags 0x%x)", adapterIndex, qPrintable(name), desc.Flags);
    }

    if (qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX")) {
        const int adapterIndex = qEnvironmentVariableIntValue("QT_D3D_ADAPTER_INDEX");
        if (SUCCEEDED(factory->EnumAdapters1(adapterIndex, &adapter))
                && SUCCEEDED(D3D12CreateDevice(adapter.Get(), fl, _uuidof(ID3D12Device), nullptr))) {
            adapter->GetDesc1(&desc);
            const QString name = QString::fromUtf16((char16_t *) desc.Description);
            qDebug("Using requested adapter '%s'", qPrintable(name));
            *outAdapter = adapter.Detach();
            return;
        }
    }

    for (int adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), fl, _uuidof(ID3D12Device), nullptr))) {
            const QString name = QString::fromUtf16((char16_t *) desc.Description);
            qDebug("Using adapter '%s'", qPrintable(name));
            break;
        }
    }

    *outAdapter = adapter.Detach();
}

ID3D12Device *QSGD3D12DeviceManager::ref()
{
    ensureCreated();
    m_ref.ref();
    return m_device.Get();
}

void QSGD3D12DeviceManager::unref()
{
    if (!m_ref.deref()) {
        if (Q_UNLIKELY(debug_render()))
            qDebug("destroying d3d device");
        m_device = nullptr;
        m_factory = nullptr;
    }
}

void QSGD3D12DeviceManager::deviceLossDetected()
{
    for (DeviceLossObserver *observer : qAsConst(m_observers))
        observer->deviceLost();

    // Nothing else to do here. All windows are expected to release their
    // resources and call unref() in response immediately.
}

IDXGIFactory4 *QSGD3D12DeviceManager::dxgi()
{
    ensureCreated();
    return m_factory.Get();
}

void QSGD3D12DeviceManager::ensureCreated()
{
    if (m_device)
        return;

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        qWarning("Failed to create DXGI: 0x%x", hr);
        return;
    }

    ComPtr<IDXGIAdapter1> adapter;
    getHardwareAdapter(m_factory.Get(), &adapter);

    bool warp = true;
    if (adapter) {
        HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        if (SUCCEEDED(hr))
            warp = false;
        else
            qWarning("Failed to create device: 0x%x", hr);
    }

    if (warp) {
        qDebug("Using WARP");
        ComPtr<IDXGIAdapter> warpAdapter;
        m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
        HRESULT hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        if (FAILED(hr)) {
            qWarning("Failed to create WARP device: 0x%x", hr);
            return;
        }
    }
}

void QSGD3D12DeviceManager::registerDeviceLossObserver(DeviceLossObserver *observer)
{
    if (!m_observers.contains(observer))
        m_observers.append(observer);
}

QSGD3D12Engine::QSGD3D12Engine()
{
    d = new QSGD3D12EnginePrivate;
}

QSGD3D12Engine::~QSGD3D12Engine()
{
    d->releaseResources();
    delete d;
}

bool QSGD3D12Engine::attachToWindow(QWindow *window)
{
    if (d->isInitialized()) {
        qWarning("QSGD3D12Engine: Cannot attach active engine to window");
        return false;
    }

    d->initialize(window);
    return d->isInitialized();
}

void QSGD3D12Engine::releaseResources()
{
    d->releaseResources();
}

void QSGD3D12Engine::resize()
{
    d->resize();
}

void QSGD3D12Engine::beginFrame()
{
    d->beginFrame();
}

void QSGD3D12Engine::endFrame()
{
    d->endFrame();
}

void QSGD3D12Engine::finalizePipeline(const QSGD3D12PipelineState &pipelineState)
{
    d->finalizePipeline(pipelineState);
}

void QSGD3D12Engine::setVertexBuffer(const quint8 *data, int size)
{
    d->setVertexBuffer(data, size);
}

void QSGD3D12Engine::setIndexBuffer(const quint8 *data, int size)
{
    d->setIndexBuffer(data, size);
}

void QSGD3D12Engine::setConstantBuffer(const quint8 *data, int size)
{
    d->setConstantBuffer(data, size);
}

void QSGD3D12Engine::markConstantBufferDirty(int offset, int size)
{
    d->markConstantBufferDirty(offset, size);
}

void QSGD3D12Engine::queueViewport(const QRect &rect)
{
    d->queueViewport(rect);
}

void QSGD3D12Engine::queueScissor(const QRect &rect)
{
    d->queueScissor(rect);
}

void QSGD3D12Engine::queueSetRenderTarget()
{
    d->queueSetRenderTarget();
}

void QSGD3D12Engine::queueClearRenderTarget(const QColor &color)
{
    d->queueClearRenderTarget(color);
}

void QSGD3D12Engine::queueClearDepthStencil(float depthValue, quint8 stencilValue, ClearFlags which)
{
    d->queueClearDepthStencil(depthValue, stencilValue, which);
}

void QSGD3D12Engine::queueSetStencilRef(quint32 ref)
{
    d->queueSetStencilRef(ref);
}

void QSGD3D12Engine::queueDraw(QSGGeometry::DrawingMode mode, int count, int vboOffset, int vboSize, int vboStride,
                               int cboOffset,
                               int startIndexIndex, QSGD3D12Format indexFormat)
{
    d->queueDraw(mode, count, vboOffset, vboSize, vboStride, cboOffset, startIndexIndex, indexFormat);
}

void QSGD3D12Engine::present()
{
    d->present();
}

void QSGD3D12Engine::waitGPU()
{
    d->waitGPU();
}

uint QSGD3D12Engine::createTexture(QImage::Format format, const QSize &size, TextureCreateFlags flags)
{
    return d->createTexture(format, size, flags);
}

void QSGD3D12Engine::releaseTexture(uint id)
{
    d->releaseTexture(id);
}

SIZE_T QSGD3D12Engine::textureSRV(uint id) const
{
    return d->textureSRV(id);
}

void QSGD3D12Engine::queueTextureUpload(uint id, const QImage &image, TextureUploadFlags flags)
{
    return d->queueTextureUpload(id, image, flags);
}

void QSGD3D12Engine::activateTexture(uint id)
{
    d->activateTexture(id);
}

quint32 QSGD3D12Engine::alignedConstantBufferSize(quint32 size)
{
    return (size + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
}

QSGD3D12Format QSGD3D12Engine::toDXGIFormat(QSGGeometry::Type sgtype, int tupleSize, int *size)
{
    QSGD3D12Format format = FmtUnknown;

    static const QSGD3D12Format formatMap_ub[] = { FmtUnknown,
                                                   FmtUNormByte,
                                                   FmtUNormByte2,
                                                   FmtUnknown,
                                                   FmtUNormByte4 };

    static const QSGD3D12Format formatMap_f[] = { FmtUnknown,
                                                  FmtFloat,
                                                  FmtFloat2,
                                                  FmtFloat3,
                                                  FmtFloat4 };

    switch (sgtype) {
    case QSGGeometry::TypeUnsignedByte:
        format = formatMap_ub[tupleSize];
        if (size)
            *size = tupleSize;
        break;
    case QSGGeometry::TypeFloat:
        format = formatMap_f[tupleSize];
        if (size)
            *size = sizeof(float) * tupleSize;
        break;

    case QSGGeometry::TypeUnsignedShort:
        format = FmtUnsignedShort;
        if (size)
            *size = sizeof(ushort) * tupleSize;
        break;
    case QSGGeometry::TypeUnsignedInt:
        format = FmtUnsignedInt;
        if (size)
            *size = sizeof(uint) * tupleSize;
        break;

    case QSGGeometry::TypeByte:
    case QSGGeometry::TypeInt:
    case QSGGeometry::TypeShort:
        qWarning("no mapping for GL type 0x%x", sgtype);
        break;

    default:
        qWarning("unknown GL type 0x%x", sgtype);
        break;
    }

    return format;
}

void QSGD3D12EnginePrivate::releaseResources()
{
    if (!initialized)
        return;

    commandAllocator = nullptr;
    copyCommandAllocator = nullptr;

    commandList = nullptr;
    copyCommandList = nullptr;

    depthStencil = nullptr;
    for (int i = 0; i < swapChainBufferCount; ++i)
        renderTargets[i] = nullptr;

    vertexBuffer = nullptr;
    indexBuffer = nullptr;
    constantBuffer = nullptr;

    psoCache.clear();
    rootSigCache.clear();
    textures.clear();

    gpuCbvSrvUavHeap = nullptr;
    cpuDescHeapManager.releaseResources();

    commandQueue = nullptr;
    copyCommandQueue = nullptr;
    swapChain = nullptr;

    delete presentFence;
    textureUploadFence = nullptr;

    deviceManager()->unref();

    initialized = false;

    // 'window' must be kept, may just be a device loss
}

void QSGD3D12EnginePrivate::initialize(QWindow *w)
{
    if (initialized)
        return;

    window = w;

    HWND hwnd = reinterpret_cast<HWND>(window->winId());

    if (qEnvironmentVariableIntValue("QT_D3D_DEBUG") != 0) {
        qDebug("Enabling debug layer");
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            debugController->EnableDebugLayer();
    }

    QSGD3D12DeviceManager *dev = deviceManager();
    device = dev->ref();
    dev->registerDeviceLossObserver(this);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)))) {
        qWarning("Failed to create command queue");
        return;
    }

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyCommandQueue)))) {
        qWarning("Failed to create copy command queue");
        return;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = swapChainBufferCount;
    swapChainDesc.BufferDesc.Width = window->width() * window->devicePixelRatio();
    swapChainDesc.BufferDesc.Height = window->height() * window->devicePixelRatio();
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // D3D12 requires the flip model
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1; // Flip does not support MSAA so no choice here
    swapChainDesc.Windowed = TRUE;

    ComPtr<IDXGISwapChain> baseSwapChain;
    HRESULT hr = dev->dxgi()->CreateSwapChain(commandQueue.Get(), &swapChainDesc, &baseSwapChain);
    if (FAILED(hr)) {
        qWarning("Failed to create swap chain: 0x%x", hr);
        return;
    }
    if (FAILED(baseSwapChain.As(&swapChain))) {
        qWarning("Failed to cast swap chain");
        return;
    }

    dev->dxgi()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)))) {
        qWarning("Failed to create command allocator");
        return;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyCommandAllocator)))) {
        qWarning("Failed to create copy command allocator");
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC gpuDescHeapDesc = {};
    gpuDescHeapDesc.NumDescriptors = MAX_GPU_CBVSRVUAV_DESCRIPTORS;
    gpuDescHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpuDescHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&gpuDescHeapDesc, IID_PPV_ARGS(&gpuCbvSrvUavHeap)))) {
        qWarning("Failed to create shader-visible CBV-SRV-UAV heap");
        return;
    }

    cpuDescHeapManager.initialize(device);

    setupRenderTargets();

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(),
                                         nullptr, IID_PPV_ARGS(&commandList)))) {
        qWarning("Failed to create command list");
        return;
    }
    // created in recording state, close it for now
    commandList->Close();

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyCommandAllocator.Get(),
                                         nullptr, IID_PPV_ARGS(&copyCommandList)))) {
        qWarning("Failed to create copy command list");
        return;
    }
    copyCommandList->Close();

    presentFence = createCPUWaitableFence();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&textureUploadFence)))) {
        qWarning("Failed to create fence");
        return;
    }

    vertexData = VICBufferRef();
    indexData = VICBufferRef();
    constantData = VICBufferRef();

    psoCache.setMaxCost(MAX_CACHED_PSO);
    rootSigCache.setMaxCost(MAX_CACHED_ROOTSIG);

    initialized = true;
}

DXGI_SAMPLE_DESC QSGD3D12EnginePrivate::makeSampleDesc(DXGI_FORMAT format, int samples)
{
    DXGI_SAMPLE_DESC sampleDesc;
    sampleDesc.Count = 1;
    sampleDesc.Quality = 0;

    if (samples > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaaInfo = {};
        msaaInfo.Format = format;
        msaaInfo.SampleCount = samples;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaaInfo, sizeof(msaaInfo)))) {
            if (msaaInfo.NumQualityLevels > 0) {
                sampleDesc.Count = samples;
                sampleDesc.Quality = msaaInfo.NumQualityLevels - 1;
            } else {
                qWarning("No quality levels for multisampling?");
            }
        } else {
            qWarning("Failed to query multisample quality levels");
        }
    }

    return sampleDesc;
}

ID3D12Resource *QSGD3D12EnginePrivate::createDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE viewHandle, const QSize &size, int samples)
{
    D3D12_CLEAR_VALUE depthClearValue = {};
    depthClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthClearValue.DepthStencil.Depth = 1.0f;
    depthClearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bufDesc.Width = size.width();
    bufDesc.Height = size.height();
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    bufDesc.SampleDesc = makeSampleDesc(bufDesc.Format, samples);
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    ID3D12Resource *resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue, IID_PPV_ARGS(&resource)))) {
        qWarning("Failed to create depth-stencil buffer of size %dx%d", size.width(), size.height());
        return nullptr;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
    depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilDesc.ViewDimension = bufDesc.SampleDesc.Count <= 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;

    device->CreateDepthStencilView(resource, &depthStencilDesc, viewHandle);

    return resource;
}

void QSGD3D12EnginePrivate::setupRenderTargets()
{
    for (int i = 0; i < swapChainBufferCount; ++i) {
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])))) {
            qWarning("Failed to get buffer %d from swap chain", i);
            return;
        }
        rtv[i] = cpuDescHeapManager.allocate(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtv[i]);
    }

    dsv = cpuDescHeapManager.allocate(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ID3D12Resource *ds = createDepthStencil(dsv, window->size(), 0);
    if (ds)
        depthStencil.Attach(ds);
}

void QSGD3D12EnginePrivate::resize()
{
    if (!initialized)
        return;

    if (Q_UNLIKELY(debug_render()))
        qDebug() << window->size();

    // Clear these, otherwise resizing will fail.
    depthStencil = nullptr;
    cpuDescHeapManager.release(dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (int i = 0; i < swapChainBufferCount; ++i) {
        renderTargets[i] = nullptr;
        cpuDescHeapManager.release(rtv[i], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    HRESULT hr = swapChain->ResizeBuffers(swapChainBufferCount, window->width(), window->height(), DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceManager()->deviceLossDetected();
        return;
    } else if (FAILED(hr)) {
        qWarning("Failed to resize buffers: 0x%x", hr);
        return;
    }

    setupRenderTargets();
}

void QSGD3D12EnginePrivate::deviceLost()
{
    qWarning("D3D device lost, will attempt to reinitialize");

    // Release all resources. This is important because otherwise reinitialization may fail.
    releaseResources();

    // Now in uninitialized state (but 'window' is still valid). Will recreate
    // all the resources on the next beginFrame().
}

QSGD3D12CPUWaitableFence *QSGD3D12EnginePrivate::createCPUWaitableFence() const
{
    QSGD3D12CPUWaitableFence *f = new QSGD3D12CPUWaitableFence;
    HRESULT hr = device->CreateFence(f->value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f->fence));
    if (FAILED(hr)) {
        qWarning("Failed to create fence: 0x%x", hr);
        return f;
    }
    f->event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return f;
}

void QSGD3D12EnginePrivate::waitForGPU(QSGD3D12CPUWaitableFence *f) const
{
    const UINT64 newValue = f->value.fetchAndAddAcquire(1) + 1;
    commandQueue->Signal(f->fence.Get(), newValue);
    if (f->fence->GetCompletedValue() < newValue) {
        HRESULT hr = f->fence->SetEventOnCompletion(newValue, f->event);
        if (FAILED(hr)) {
            qWarning("SetEventOnCompletion failed: 0x%x", hr);
            return;
        }
        WaitForSingleObject(f->event, INFINITE);
    }
}

void QSGD3D12EnginePrivate::transitionResource(ID3D12Resource *resource, ID3D12GraphicsCommandList *commandList,
                                               D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    D3D12_RESOURCE_BARRIER barrier;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    commandList->ResourceBarrier(1, &barrier);
}

ID3D12Resource *QSGD3D12EnginePrivate::createBuffer(int size)
{
    ID3D12Resource *buf;

    D3D12_HEAP_PROPERTIES uploadHeapProp = {};
    uploadHeapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = size;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(&uploadHeapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr))
        qWarning("Failed to create buffer resource: 0x%x", hr);

    return buf;
}

ID3D12Resource *QSGD3D12EnginePrivate::backBufferRT() const
{
    return renderTargets[swapChain->GetCurrentBackBufferIndex()].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE QSGD3D12EnginePrivate::backBufferRTV() const
{
    const int frameIndex = swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtv[0];
    rtvHandle.ptr += frameIndex * cpuDescHeapManager.handleSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rtvHandle;
}

void QSGD3D12EnginePrivate::beginFrame()
{
    if (inFrame)
        qWarning("beginFrame called again without an endFrame");

    inFrame = true;

    if (Q_UNLIKELY(debug_render())) {
        static int cnt = 0;
        qDebug() << "***** begin frame" << cnt;
        ++cnt;
    }

    // The device may have been lost. This is the point to attempt to start again from scratch.
    if (!initialized && window)
        initialize(window);

    commandAllocator->Reset();

    cbvSrvUavNextFreeDescriptorIndex = 0;

    beginDrawCalls(true);
}

void QSGD3D12EnginePrivate::beginDrawCalls(bool needsBackbufferTransition)
{
    commandList->Reset(commandAllocator.Get(), nullptr);

    frameData.drawingMode = QSGGeometry::DrawingMode(-1);
    frameData.indexBufferSet = false;
    frameData.drawCount = 0;

    if (needsBackbufferTransition)
        transitionResource(backBufferRT(), commandList.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void QSGD3D12EnginePrivate::updateBuffer(VICBufferRef *br, ID3D12Resource *r, const char *dbgstr)
{
    quint8 *p = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    if (!br->dirty.isEmpty()) {
        if (FAILED(r->Map(0, &readRange, reinterpret_cast<void **>(&p)))) {
            qWarning("Map failed for %s buffer of size %d", dbgstr, br->size);
            return;
        }
        for (const auto &r : qAsConst(br->dirty)) {
            if (Q_UNLIKELY(debug_render()))
                qDebug("%s o %d s %d", dbgstr, r.first, r.second);
            memcpy(p + r.first, br->p + r.first, r.second);
        }
        r->Unmap(0, nullptr);
        br->dirty.clear();
    }
}

void QSGD3D12EnginePrivate::endFrame()
{
    if (Q_UNLIKELY(debug_render()))
        qDebug() << "***** end frame";

    endDrawCalls(true);

    inFrame = false;
}

void QSGD3D12EnginePrivate::endDrawCalls(bool needsBackbufferTransition)
{
    // Now is the time to sync all the changed areas in the buffers.
    updateBuffer(&vertexData, vertexBuffer.Get(), "vertex");
    updateBuffer(&indexData, indexBuffer.Get(), "index");
    updateBuffer(&constantData, constantBuffer.Get(), "constant");

    // Wait for texture uploads to finish.
    quint64 topFenceValue = 0;
    for (uint id : qAsConst(frameData.pendingTextures)) {
        const int idx = id - 1;
        Q_ASSERT(idx < textures.count());
        const Texture &t(textures[idx]);
        if (t.fenceValue)
            topFenceValue = qMax(topFenceValue, t.fenceValue);
    }
    if (topFenceValue) {
        if (Q_UNLIKELY(debug_render()))
            qDebug("wait for texture fence %llu", topFenceValue);
        commandQueue->Wait(textureUploadFence.Get(), topFenceValue);
    }
    for (uint id : qAsConst(frameData.pendingTextures)) {
        const int idx = id - 1;
        Texture &t(textures[idx]);
        if (t.fenceValue) {
            t.fenceValue = 0;
            t.stagingBuffer = nullptr;
        }
    }
    if (topFenceValue) {
        bool hasActiveTextureUpload = false;
        for (const Texture &t : qAsConst(textures)) {
            if (t.fenceValue) {
                hasActiveTextureUpload = true;
                break;
            }
        }
        if (!hasActiveTextureUpload)
            copyCommandAllocator->Reset();
    }
    frameData.pendingTextures.clear();

    if (needsBackbufferTransition)
        transitionResource(backBufferRT(), commandList.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    HRESULT hr = commandList->Close();
    if (FAILED(hr)) {
        qWarning("Failed to close command list: 0x%x", hr);
        if (hr == E_INVALIDARG)
            qWarning("Invalid arguments. Some of the commands in the list is invalid in some way.");
    }

    ID3D12CommandList *commandLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
}

// Root signature:
// [0] CBV - always present
// [1] table with 1 SRV per texture (optional)
// one constant sampler per texture (optional)
//
// SRVs can be created freely via QSGD3D12CPUDescriptorHeapManager and stored
// in QSGD3D12TextureView. The engine will copy them onto a dedicated,
// shader-visible CBV-SRV-UAV heap.

void QSGD3D12EnginePrivate::finalizePipeline(const QSGD3D12PipelineState &pipelineState)
{
    frameData.pipelineState = pipelineState;

    RootSigCacheEntry *cachedRootSig = rootSigCache[pipelineState.shaders.rootSig];
    if (!cachedRootSig) {
        if (Q_UNLIKELY(debug_render()))
            qDebug("NEW ROOTSIG");

        cachedRootSig = new RootSigCacheEntry;

        D3D12_ROOT_PARAMETER rootParams[4];
        int rootParamCount = 0;

        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[0].Descriptor.ShaderRegister = 0; // b0
        rootParams[0].Descriptor.RegisterSpace = 0;
        ++rootParamCount;

        if (!pipelineState.shaders.rootSig.textureViews.isEmpty()) {
            rootParams[rootParamCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[rootParamCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rootParams[rootParamCount].DescriptorTable.NumDescriptorRanges = 1;
            D3D12_DESCRIPTOR_RANGE descRange;
            descRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            descRange.NumDescriptors = pipelineState.shaders.rootSig.textureViews.count();
            descRange.BaseShaderRegister = 0; // t0, t1, ...
            descRange.RegisterSpace = 0;
            descRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            rootParams[rootParamCount].DescriptorTable.pDescriptorRanges = &descRange;
            ++rootParamCount;
        }

        Q_ASSERT(rootParamCount <= _countof(rootParams));
        D3D12_ROOT_SIGNATURE_DESC desc;
        desc.NumParameters = rootParamCount;
        desc.pParameters = rootParams;
        desc.NumStaticSamplers = pipelineState.shaders.rootSig.textureViews.count();
        D3D12_STATIC_SAMPLER_DESC staticSamplers[8];
        int sdIdx = 0;
        Q_ASSERT(pipelineState.shaders.rootSig.textureViews.count() <= _countof(staticSamplers));
        for (const QSGD3D12TextureView &tv : qAsConst(pipelineState.shaders.rootSig.textureViews)) {
            D3D12_STATIC_SAMPLER_DESC sd = {};
            sd.Filter = D3D12_FILTER(tv.filter);
            sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE(tv.addressModeHoriz);
            sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE(tv.addressModeVert);
            sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MinLOD = 0.0f;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            sd.ShaderRegister = sdIdx; // t0, t1, ...
            sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            staticSamplers[sdIdx++] = sd;
        }
        desc.pStaticSamplers = staticSamplers;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
            qWarning("Failed to serialize root signature");
            return;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                               IID_PPV_ARGS(&cachedRootSig->rootSig)))) {
            qWarning("Failed to create root signature");
            return;
        }

        rootSigCache.insert(pipelineState.shaders.rootSig, cachedRootSig);
    }

    PSOCacheEntry *cachedPso = psoCache[pipelineState];
    if (!cachedPso) {
        if (Q_UNLIKELY(debug_render()))
            qDebug("NEW PSO");

        cachedPso = new PSOCacheEntry;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

        D3D12_INPUT_ELEMENT_DESC inputElements[8];
        Q_ASSERT(pipelineState.inputElements.count() <= _countof(inputElements));
        int ieIdx = 0;
        for (const QSGD3D12InputElement &ie : pipelineState.inputElements) {
            D3D12_INPUT_ELEMENT_DESC ieDesc = {};
            ieDesc.SemanticName = ie.semanticName;
            ieDesc.SemanticIndex = ie.semanticIndex;
            ieDesc.Format = DXGI_FORMAT(ie.format);
            ieDesc.InputSlot = ie.slot;
            ieDesc.AlignedByteOffset = ie.offset;
            ieDesc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            if (Q_UNLIKELY(debug_render()))
                qDebug("input [%d]: %s %d 0x%x %d", ieIdx, ie.semanticName, ie.offset, ie.format, ie.slot);
            inputElements[ieIdx++] = ieDesc;
        }

        psoDesc.InputLayout = { inputElements, UINT(ieIdx) };

        psoDesc.pRootSignature = cachedRootSig->rootSig.Get();

        D3D12_SHADER_BYTECODE vshader;
        vshader.pShaderBytecode = pipelineState.shaders.vs;
        vshader.BytecodeLength = pipelineState.shaders.vsSize;
        D3D12_SHADER_BYTECODE pshader;
        pshader.pShaderBytecode = pipelineState.shaders.ps;
        pshader.BytecodeLength = pipelineState.shaders.psSize;

        psoDesc.VS = vshader;
        psoDesc.PS = pshader;

        D3D12_RASTERIZER_DESC rastDesc = {};
        rastDesc.FillMode = D3D12_FILL_MODE_SOLID;
        rastDesc.CullMode = D3D12_CULL_MODE(pipelineState.cullMode);
        rastDesc.FrontCounterClockwise = pipelineState.frontCCW;
        rastDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        rastDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        rastDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        rastDesc.DepthClipEnable = TRUE;

        psoDesc.RasterizerState = rastDesc;

        D3D12_BLEND_DESC blendDesc = {};
        if (pipelineState.premulBlend) {
            const D3D12_RENDER_TARGET_BLEND_DESC premulBlendDesc = {
                TRUE, FALSE,
                D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
                D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                D3D12_LOGIC_OP_NOOP,
                UINT8(pipelineState.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0)
            };
            blendDesc.RenderTarget[0] = premulBlendDesc;
        } else {
            D3D12_RENDER_TARGET_BLEND_DESC noBlendDesc = {};
            noBlendDesc.RenderTargetWriteMask = pipelineState.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
            blendDesc.RenderTarget[0] = noBlendDesc;
        }
        psoDesc.BlendState = blendDesc;

        psoDesc.DepthStencilState.DepthEnable = pipelineState.depthEnable;
        psoDesc.DepthStencilState.DepthWriteMask = pipelineState.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(pipelineState.depthFunc);

        psoDesc.DepthStencilState.StencilEnable = pipelineState.stencilEnable;
        psoDesc.DepthStencilState.StencilReadMask = psoDesc.DepthStencilState.StencilWriteMask = 0xFF;
        D3D12_DEPTH_STENCILOP_DESC stencilOpDesc = {
            D3D12_STENCIL_OP(pipelineState.stencilFailOp),
            D3D12_STENCIL_OP(pipelineState.stencilDepthFailOp),
            D3D12_STENCIL_OP(pipelineState.stencilPassOp),
            D3D12_COMPARISON_FUNC(pipelineState.stencilFunc)
        };
        psoDesc.DepthStencilState.FrontFace = psoDesc.DepthStencilState.BackFace = stencilOpDesc;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(pipelineState.topologyType);
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        psoDesc.SampleDesc.Count = 1;

        HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&cachedPso->pso));
        if (FAILED(hr)) {
            qWarning("Failed to create graphics pipeline state");
            return;
        }

        psoCache.insert(pipelineState, cachedPso);
    }

    commandList->SetPipelineState(cachedPso->pso.Get());

    commandList->SetGraphicsRootSignature(cachedRootSig->rootSig.Get()); // invalidates bindings

    if (!pipelineState.shaders.rootSig.textureViews.isEmpty()) {
        ID3D12DescriptorHeap *heaps[] = { gpuCbvSrvUavHeap.Get() };
        commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    }
}

void QSGD3D12EnginePrivate::setVertexBuffer(const quint8 *data, int size)
{
    vertexData.p = data;
    vertexData.size = size;
    vertexData.fullChange = true;
}

void QSGD3D12EnginePrivate::setIndexBuffer(const quint8 *data, int size)
{
    indexData.p = data;
    indexData.size = size;
    indexData.fullChange = true;
}

void QSGD3D12EnginePrivate::setConstantBuffer(const quint8 *data, int size)
{
    constantData.p = data;
    constantData.size = size;
    constantData.fullChange = true;
}

void QSGD3D12EnginePrivate::markConstantBufferDirty(int offset, int size)
{
    const QPair<int, int> range = qMakePair(offset, size);
    // don't bother checking for overlapping dirty ranges, it won't happen with the current renderer
    if (!constantData.dirty.contains(range))
        constantData.dirty.append(range);
}

void QSGD3D12EnginePrivate::queueViewport(const QRect &rect)
{
    frameData.viewport = rect;
    const D3D12_VIEWPORT viewport = { float(rect.x()), float(rect.y()), float(rect.width()), float(rect.height()), 0, 1 };
    commandList->RSSetViewports(1, &viewport);
}

void QSGD3D12EnginePrivate::queueScissor(const QRect &rect)
{
    frameData.scissor = rect;
    const D3D12_RECT scissorRect = { rect.left(), rect.top(), rect.right(), rect.bottom() };
    commandList->RSSetScissorRects(1, &scissorRect);
}

void QSGD3D12EnginePrivate::queueSetRenderTarget()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = backBufferRTV();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsv;
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
}

void QSGD3D12EnginePrivate::queueClearRenderTarget(const QColor &color)
{
    const float clearColor[] = { float(color.redF()), float(color.blueF()), float(color.greenF()), float(color.alphaF()) };
    commandList->ClearRenderTargetView(backBufferRTV(), clearColor, 0, nullptr);
}

void QSGD3D12EnginePrivate::queueClearDepthStencil(float depthValue, quint8 stencilValue, QSGD3D12Engine::ClearFlags which)
{
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAGS(int(which)), depthValue, stencilValue, 0, nullptr);
}

void QSGD3D12EnginePrivate::queueSetStencilRef(quint32 ref)
{
    frameData.stencilRef = ref;
    commandList->OMSetStencilRef(ref);
}

void QSGD3D12EnginePrivate::queueDraw(QSGGeometry::DrawingMode mode, int count, int vboOffset, int vboSize, int vboStride,
                                      int cboOffset,
                                      int startIndexIndex, QSGD3D12Format indexFormat)
{
    // Ensure buffers are created but do not copy the data here, leave that to endDrawCalls().
    if (vertexData.fullChange) {
        vertexData.fullChange = false;
        // Only enlarge, never shrink
        const bool newBufferNeeded = vertexBuffer ? (vertexData.size > vertexBuffer->GetDesc().Width) : true;
        if (newBufferNeeded) {
            if (Q_UNLIKELY(debug_render()))
                qDebug("new vertex buffer of size %d", vertexData.size);
            vertexBuffer.Attach(createBuffer(vertexData.size));
        }
        vertexData.dirty.clear();
        if (vertexBuffer)
            vertexData.dirty.append(qMakePair(0, vertexData.size));
        else
            return;
    }

    if (indexData.fullChange) {
        indexData.fullChange = false;
        if (indexData.size > 0) {
            const bool newBufferNeeded = indexBuffer ? (indexData.size > indexBuffer->GetDesc().Width) : true;
            if (newBufferNeeded) {
                if (Q_UNLIKELY(debug_render()))
                    qDebug("new index buffer of size %d", indexData.size);
                indexBuffer.Attach(createBuffer(indexData.size));
            }
            indexData.dirty.clear();
            if (indexBuffer)
                indexData.dirty.append(qMakePair(0, indexData.size));
            else
                return;
        } else {
            indexBuffer = nullptr;
        }
    }

    if (constantData.fullChange) {
        constantData.fullChange = false;
        const bool newBufferNeeded = constantBuffer ? (constantData.size > constantBuffer->GetDesc().Width) : true;
        if (newBufferNeeded) {
            if (Q_UNLIKELY(debug_render()))
                qDebug("new constant buffer of size %d", constantData.size);
            constantBuffer.Attach(createBuffer(constantData.size));
        }
        constantData.dirty.clear();
        if (constantBuffer)
            constantData.dirty.append(qMakePair(0, constantData.size));
        else
            return;
    }

    // Set the CBV.
    if (cboOffset >= 0 && constantBuffer)
        commandList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress() + cboOffset);

    // Set up vertex and index buffers.
    Q_ASSERT(vertexBuffer);
    Q_ASSERT(indexBuffer || startIndexIndex < 0);

    if (mode != frameData.drawingMode) {
        D3D_PRIMITIVE_TOPOLOGY topology;
        switch (mode) {
        case QSGGeometry::DrawPoints:
            topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case QSGGeometry::DrawLines:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case QSGGeometry::DrawLineStrip:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case QSGGeometry::DrawTriangles:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case QSGGeometry::DrawTriangleStrip:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        default:
            qFatal("Unsupported drawing mode 0x%x", mode);
            break;
        }
        commandList->IASetPrimitiveTopology(topology);
        frameData.drawingMode = mode;
    }

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = vertexBuffer->GetGPUVirtualAddress() + vboOffset;
    vbv.SizeInBytes = vboSize;
    vbv.StrideInBytes = vboStride;

    // must be set after the topology
    commandList->IASetVertexBuffers(0, 1, &vbv);

    if (startIndexIndex >= 0 && !frameData.indexBufferSet) {
        frameData.indexBufferSet = true;
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        ibv.SizeInBytes = indexData.size;
        ibv.Format = DXGI_FORMAT(indexFormat);
        commandList->IASetIndexBuffer(&ibv);
    }

    // Copy the SRVs to a drawcall-dedicated area of the shader-visible descriptor heap.
    Q_ASSERT(frameData.activeTextures.count() == frameData.pipelineState.shaders.rootSig.textureViews.count());
    if (!frameData.activeTextures.isEmpty()) {
        const uint stride = cpuDescHeapManager.handleSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE dst = gpuCbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        if (cbvSrvUavNextFreeDescriptorIndex + frameData.activeTextures.count() > MAX_GPU_CBVSRVUAV_DESCRIPTORS) {
            // oops.
            // ### figure out something
            qFatal("Out of space for shader-visible SRVs");
        }
        dst.ptr += cbvSrvUavNextFreeDescriptorIndex * stride;
        for (uint id : qAsConst(frameData.activeTextures)) {
            const int idx = id - 1;
            device->CopyDescriptorsSimple(1, dst, textures[idx].srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            dst.ptr += stride;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE gpuAddr = gpuCbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
        gpuAddr.ptr += cbvSrvUavNextFreeDescriptorIndex * stride;
        commandList->SetGraphicsRootDescriptorTable(1, gpuAddr);

        cbvSrvUavNextFreeDescriptorIndex += frameData.activeTextures.count();
        frameData.activeTextures.clear();
    }

    // Add the draw call.
    if (startIndexIndex >= 0)
        commandList->DrawIndexedInstanced(count, 1, startIndexIndex, 0, 0);
    else
        commandList->DrawInstanced(count, 1, 0, 0);

    ++frameData.drawCount;
    if (frameData.drawCount == MAX_DRAW_CALLS_PER_LIST) {
        if (Q_UNLIKELY(debug_render()))
            qDebug("Limit of %d draw calls reached, executing command list", MAX_DRAW_CALLS_PER_LIST);
        // submit the command list
        endDrawCalls();
        // start a new one
        beginDrawCalls();
        // prepare for the upcoming drawcalls
        queueSetRenderTarget();
        queueViewport(frameData.viewport);
        queueScissor(frameData.scissor);
        queueSetStencilRef(frameData.stencilRef);
        finalizePipeline(frameData.pipelineState);
    }
}

void QSGD3D12EnginePrivate::present()
{
    if (!initialized)
        return;

    if (Q_UNLIKELY(debug_render()))
        qDebug("--- present with vsync ---");

    HRESULT hr = swapChain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceManager()->deviceLossDetected();
        return;
    } else if (FAILED(hr)) {
        qWarning("Present failed: 0x%x", hr);
        return;
    }
}

void QSGD3D12EnginePrivate::waitGPU()
{
    if (!initialized)
        return;

    if (Q_UNLIKELY(debug_render()))
        qDebug("--- blocking wait for GPU ---");

    waitForGPU(presentFence);
}

uint QSGD3D12EnginePrivate::createTexture(QImage::Format format, const QSize &size, QSGD3D12Engine::TextureCreateFlags flags)
{
    int id = 0;
    for (int i = 0; i < textures.count(); ++i) {
        if (!textures[i].texture) {
            id = i + 1;
            break;
        }
    }
    if (!id) {
        textures.resize(textures.size() + 1);
        id = textures.count();
    }

    const int idx = id - 1;
    Texture &t(textures[idx]);

    D3D12_HEAP_PROPERTIES defaultHeapProp = {};
    defaultHeapProp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = size.width();
    textureDesc.Height = size.height();
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1; // ###
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // ### use format
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = device->CreateCommittedResource(&defaultHeapProp, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.texture));
    if (FAILED(hr)) {
        qWarning("Failed to create texture resource: 0x%x", hr);
        return 0;
    }

    t.srv = cpuDescHeapManager.allocate(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1; // ###

    device->CreateShaderResourceView(t.texture.Get(), &srvDesc, t.srv);

    if (Q_UNLIKELY(debug_render()))
        qDebug("allocated texture %d", id);

    return id;
}

void QSGD3D12EnginePrivate::releaseTexture(uint id)
{
    if (!id)
        return;

    const int idx = id - 1;
    Q_ASSERT(idx < textures.count());

    if (Q_UNLIKELY(debug_render()))
        qDebug("releasing texture %d", id);

    textures[idx].texture = nullptr;
    cpuDescHeapManager.release(textures[idx].srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

SIZE_T QSGD3D12EnginePrivate::textureSRV(uint id) const
{
    Q_ASSERT(id);
    const int idx = id - 1;
    Q_ASSERT(idx < textures.count());
    return textures[idx].srv.ptr;
}

void QSGD3D12EnginePrivate::queueTextureUpload(uint id, const QImage &image, QSGD3D12Engine::TextureUploadFlags flags)
{
    Q_ASSERT(id);
    const int idx = id - 1;
    Q_ASSERT(idx < textures.count());

    Texture &t(textures[idx]);
    if (t.fenceValue) {
        qWarning("queueTextureUpload: An upload is still active for texture %d", id);
        return;
    }
    if (!t.texture) {
        qWarning("queueTextureUpload: Attempted to upload for non-created texture %d", id);
        return;
    }

    t.fenceValue = nextTextureUploadFenceValue.fetchAndAddAcquire(1) + 1;

    D3D12_RESOURCE_DESC textureDesc = t.texture->GetDesc();
    UINT64 bufferSize;
    const int TEXTURE_MIP_LEVELS = 1; // ###
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureLayout[TEXTURE_MIP_LEVELS];
    device->GetCopyableFootprints(&textureDesc, 0, TEXTURE_MIP_LEVELS, 0, textureLayout, nullptr, nullptr, &bufferSize);

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = bufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeapProp = {};
    uploadHeapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    if (FAILED(device->CreateCommittedResource(&uploadHeapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&t.stagingBuffer)))) {
        qWarning("Failed to create texture upload buffer");
        return;
    }

    QImage convImage = image.convertToFormat(QImage::Format_RGBA8888); // ###

    quint8 *p = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(t.stagingBuffer->Map(0, &readRange, reinterpret_cast<void **>(&p)))) {
        qWarning("Map failed (texture upload buffer)");
        return;
    }
    quint8 *lp = p + textureLayout[0].Offset;
    for (uint y = 0; y < textureDesc.Height; ++y) {
        memcpy(lp, convImage.scanLine(y), convImage.width() * 4);
        lp += textureLayout[0].Footprint.RowPitch;
    }
    t.stagingBuffer->Unmap(0, nullptr);

    copyCommandList->Reset(copyCommandAllocator.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc;
    dstLoc.pResource = t.texture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc;
    srcLoc.pResource = t.stagingBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = textureLayout[0];
    copyCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    copyCommandList->Close();
    ID3D12CommandList *commandLists[] = { copyCommandList.Get() };
    copyCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
    copyCommandQueue->Signal(textureUploadFence.Get(), t.fenceValue);
}

void QSGD3D12EnginePrivate::activateTexture(uint id)
{
    if (!inFrame) {
        qWarning("activateTexture cannot be called outside begin/endFrame");
        return;
    }

    frameData.pendingTextures.insert(id);
    frameData.activeTextures.append(id);
}

QT_END_NAMESPACE