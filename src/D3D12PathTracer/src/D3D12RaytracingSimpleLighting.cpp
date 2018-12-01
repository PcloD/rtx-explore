//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "stdafx.h"
#include "model_loading/tiny_obj_loader.h"
#include "D3D12RaytracingSimpleLighting.h"
#include "DirectXRaytracingHelper.h"
#include "CompiledShaders\Raytracing.hlsl.h"
#include "TextureLoader.h"
#include "Mesh.h"
#include "MeshLoader.h"
#include <iostream>
#include <algorithm>
#include <glm/glm/gtc/matrix_transform.inl>

using namespace std;
using namespace DX;

const wchar_t* D3D12RaytracingSimpleLighting::c_hitGroupName = L"MyHitGroup";
const wchar_t* D3D12RaytracingSimpleLighting::c_raygenShaderName = L"MyRaygenShader";
const wchar_t* D3D12RaytracingSimpleLighting::c_closestHitShaderName = L"MyClosestHitShader";
const wchar_t* D3D12RaytracingSimpleLighting::c_missShaderName = L"MyMissShader";
const float D3D12RaytracingSimpleLighting::c_rotateDegrees = 5.f;
const float D3D12RaytracingSimpleLighting::c_movementAmountFactor = 0.1f;
const unsigned int D3D12RaytracingSimpleLighting::c_maxIteration = 6000;

D3D12RaytracingSimpleLighting::D3D12RaytracingSimpleLighting(UINT width, UINT height, std::wstring name) :
  DXSample(width, height, name),
  m_raytracingOutputResourceUAVDescriptorHeapIndex(UINT_MAX),
  m_isDxrSupported(false)
{
  m_forceComputeFallback = false;
  SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
  UpdateForSizeChange(width, height);
}

void D3D12RaytracingSimpleLighting::EnableDirectXRaytracing(IDXGIAdapter1* adapter)
{
  // Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
  bool isFallbackSupported = EnableComputeRaytracingFallback(adapter);

  if (!isFallbackSupported)
  {
    OutputDebugString(
      L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n"
      L"         Possible reasons: your OS is not in developer mode.\n\n");
  }

  m_isDxrSupported = IsDirectXRaytracingSupported(adapter);

  if (!m_isDxrSupported)
  {
    OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

    ThrowIfFalse(isFallbackSupported,
                 L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"
                 L"Possible reasons: your OS is not in developer mode.\n\n");
    m_raytracingAPI = RaytracingAPI::FallbackLayer;
  }
}

void D3D12RaytracingSimpleLighting::OnInit()
{
  m_deviceResources = std::make_unique<DeviceResources>(
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_UNKNOWN,
    FrameCount,
    D3D_FEATURE_LEVEL_11_0,
    // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since TH2.
    // Since the Fallback Layer requires Fall Creator's update (RS3), we don't need to handle non-tearing cases.
    DeviceResources::c_RequireTearingSupport,
    m_adapterIDoverride
  );
  m_deviceResources->RegisterDeviceNotify(this);
  m_deviceResources->SetWindow(Win32Application::GetHwnd(), m_width, m_height);
  m_deviceResources->InitializeDXGIAdapter();
  EnableDirectXRaytracing(m_deviceResources->GetAdapter());

  m_deviceResources->CreateDeviceResources();
  m_deviceResources->CreateWindowSizeDependentResources();

  InitializeScene();

  CreateDeviceDependentResources();
  CreateWindowSizeDependentResources();
}

// Update camera matrices passed into the shader.
void D3D12RaytracingSimpleLighting::UpdateCameraMatrices()
{
  auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

  m_sceneCB[frameIndex].cameraPosition = m_eye;
  float fovAngleY = 45.0f;

  XMMATRIX view = XMMatrixLookAtLH(m_eye, m_at, m_up);
  XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovAngleY), m_aspectRatio, 1.0f, 125.0f);
  XMMATRIX viewProj = view * proj;

  m_sceneCB[frameIndex].projectionToWorld = XMMatrixInverse(nullptr, viewProj);
}

// Initialize scene rendering parameters.
void D3D12RaytracingSimpleLighting::InitializeScene()
{
  auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

  // Setup materials.
  {
    m_cubeCB.albedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
  }

  // Setup camera.
  {
    // Initialize the view and projection inverse matrices.
    m_eye = {0.0f, 2.0f, -20.0f, 1.0f};
    m_at = {0.0f, 0.0f, 1.0f, 1.0f};

    m_right = {1.0f, 0.0f, 0.0f, 1.0f};
    m_up = {0.0f, 1.0f, 0.0f, 1.0f};
    m_forward = {0.0f, 0.0f, 1.0f, 1.0f};

    m_camChanged = true;

    UpdateCameraMatrices();
  }

  // Setup lights.
  {
    // Initialize the lighting parameters.
    XMFLOAT4 lightPosition;
    XMFLOAT4 lightAmbientColor;
    XMFLOAT4 lightDiffuseColor;

    lightPosition = XMFLOAT4(0.0f, 1.8f, -3.0f, 0.0f);
    m_sceneCB[frameIndex].lightPosition = XMLoadFloat4(&lightPosition);

    lightAmbientColor = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
    m_sceneCB[frameIndex].lightAmbientColor = XMLoadFloat4(&lightAmbientColor);

    lightDiffuseColor = XMFLOAT4(1.0, 0.5f, 0.5f, 1.0f);
    m_sceneCB[frameIndex].lightDiffuseColor = XMLoadFloat4(&lightDiffuseColor);
    m_sceneCB[frameIndex].lightDiffuseColor = XMLoadFloat4(&lightDiffuseColor);
  }

  // Setup path tracing state
  {
    m_sceneCB[frameIndex].iteration = 1;
    m_sceneCB[frameIndex].depth = 5;
  }

  // Apply the initial values to all frames' buffer instances.
  for (auto& sceneCB : m_sceneCB)
  {
    sceneCB = m_sceneCB[frameIndex];
  }
}

// Create constant buffers.
void D3D12RaytracingSimpleLighting::CreateConstantBuffers()
{
  auto device = m_deviceResources->GetD3DDevice();
  auto frameCount = m_deviceResources->GetBackBufferCount();

  // Create the constant buffer memory and map the CPU and GPU addresses
  const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

  // Allocate one constant buffer per frame, since it gets updated every frame.
  size_t cbSize = frameCount * sizeof(AlignedSceneConstantBuffer);
  const D3D12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

  ThrowIfFailed(device->CreateCommittedResource(
    &uploadHeapProperties,
    D3D12_HEAP_FLAG_NONE,
    &constantBufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    nullptr,
    IID_PPV_ARGS(&m_perFrameConstants)));

  // Map the constant buffer and cache its heap pointers.
  // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
  CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
  ThrowIfFailed(m_perFrameConstants->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstantData)));
}

// Create resources that depend on the device.
void D3D12RaytracingSimpleLighting::CreateDeviceDependentResources()
{
  // Initialize raytracing pipeline.

  // Create raytracing interfaces: raytracing device and commandlist.
  CreateRaytracingInterfaces();

  m_sceneLoaded = new Scene(p_sceneFileName, this); // this will load everything in the argument text file

  // Create root signatures for the shaders.
  CreateRootSignatures();

  // Create a raytracing pipeline state object which defines the binding of shaders, state and resources to be used during raytracing.
  CreateRaytracingPipelineStateObject();

  // Create a heap for descriptors.
  CreateDescriptorHeap();

  // Build geometry to be used in the sample.
  m_sceneLoaded->AllocateResourcesInDescriptorHeap();


  //BuildMesh("src/objects/wahoo.obj");

  // Build raytracing acceleration structures from the generated geometry.
  BuildAccelerationStructures();

  // Create constant buffers for the geometry and the scene.
  CreateConstantBuffers();

  // LOOKAT
  //CreateTexture();
  //CreateNormalTexture();

  // Build shader tables, which define shaders and their local root arguments.
  BuildShaderTables();

  // Create an output 2D texture to store the raytracing result to.
  CreateRaytracingOutputResource();
}

bool D3D12RaytracingSimpleLighting::CreateTexture()
{
  // Load the image from file
  D3D12_RESOURCE_DESC textureDesc;
  int imageBytesPerRow;
  BYTE* imageData;
  int imageSize = TextureLoader::LoadImageDataFromFile(&imageData, textureDesc, L"src/textures/wahoo.bmp",
                                                       imageBytesPerRow);

  // make sure we have data
  if (imageSize <= 0)
  {
    return false;
  }

  auto device = m_deviceResources->GetD3DDevice();

  auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

  ThrowIfFailed(device->CreateCommittedResource(
    &defaultHeapProperties,
    D3D12_HEAP_FLAG_NONE,
    &textureDesc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    nullptr,
    IID_PPV_ARGS(&m_textureBuffer.resource)));

  UINT64 textureUploadBufferSize;
  // this function gets the size an upload buffer needs to be to upload a texture to the gpu.
  // each row must be 256 byte aligned except for the last row, which can just be the size in bytes of the row
  // eg. textureUploadBufferSize = ((((width * numBytesPerPixel) + 255) & ~255) * (height - 1)) + (width * numBytesPerPixel);
  //textureUploadBufferSize = (((imageBytesPerRow + 255) & ~255) * (textureDesc.Height - 1)) + imageBytesPerRow;
  device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &textureUploadBufferSize);

  // now we create an upload heap to upload our texture to the GPU
  ThrowIfFailed(device->CreateCommittedResource(
    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // upload heap
    D3D12_HEAP_FLAG_NONE, // no flags
    &CD3DX12_RESOURCE_DESC::Buffer(textureUploadBufferSize),
    // resource description for a buffer (storing the image data in this heap just to copy to the default heap)
    D3D12_RESOURCE_STATE_GENERIC_READ, // We will copy the contents from this heap to the default heap above
    nullptr,
    IID_PPV_ARGS(&textureBufferUploadHeap)));

  textureBufferUploadHeap->SetName(L"Texture Buffer Upload Resource Heap");

  // store vertex buffer in upload heap
  D3D12_SUBRESOURCE_DATA textureData = {};
  textureData.pData = &imageData[0]; // pointer to our image data
  textureData.RowPitch = imageBytesPerRow; // size of all our triangle vertex data
  textureData.SlicePitch = imageBytesPerRow * textureDesc.Height; // also the size of our triangle vertex data

  auto commandList = m_deviceResources->GetCommandList();
  auto commandAllocator = m_deviceResources->GetCommandAllocator();

  commandList->Reset(commandAllocator, nullptr);

  // Reset the command list for the acceleration structure construction.
  UpdateSubresources(commandList, m_textureBuffer.resource.Get(), textureBufferUploadHeap, 0, 0, 1, &textureData);

  // transition the texture default heap to a pixel shader resource (we will be sampling from this heap in the pixel shader to get the color of pixels)
  commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_textureBuffer.resource.Get(),
                                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

  UINT descriptorIndex = AllocateDescriptor(&m_textureBuffer.cpuDescriptorHandle);

  // create SRV descriptor
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Format = textureDesc.Format;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(m_textureBuffer.resource.Get(), &srvDesc, m_textureBuffer.cpuDescriptorHandle);

  // used to bind to the root signature
  m_textureBuffer.gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
    m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, m_descriptorSize);

  // Kick off texture uploading
  m_deviceResources->ExecuteCommandList();

  // Wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope.
  m_deviceResources->WaitForGpu();
}

bool D3D12RaytracingSimpleLighting::CreateNormalTexture()
{
  // Load the image from file
  D3D12_RESOURCE_DESC textureDesc;
  int imageBytesPerRow;
  BYTE* imageData;
  int imageSize = TextureLoader::LoadImageDataFromFile(&imageData, textureDesc, L"src/textures/Cerberus/Cerberus_N.png",
                                                       imageBytesPerRow);

  // make sure we have data
  if (imageSize <= 0)
  {
    return false;
  }

  auto device = m_deviceResources->GetD3DDevice();

  auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

  ThrowIfFailed(device->CreateCommittedResource(
    &defaultHeapProperties,
    D3D12_HEAP_FLAG_NONE,
    &textureDesc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    nullptr,
    IID_PPV_ARGS(&m_normalTextureBuffer.resource)));

  UINT64 textureUploadBufferSize;
  // this function gets the size an upload buffer needs to be to upload a texture to the gpu.
  // each row must be 256 byte aligned except for the last row, which can just be the size in bytes of the row
  // eg. textureUploadBufferSize = ((((width * numBytesPerPixel) + 255) & ~255) * (height - 1)) + (width * numBytesPerPixel);
  //textureUploadBufferSize = (((imageBytesPerRow + 255) & ~255) * (textureDesc.Height - 1)) + imageBytesPerRow;
  device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &textureUploadBufferSize);

  // now we create an upload heap to upload our texture to the GPU
  ThrowIfFailed(device->CreateCommittedResource(
    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // upload heap
    D3D12_HEAP_FLAG_NONE, // no flags
    &CD3DX12_RESOURCE_DESC::Buffer(textureUploadBufferSize),
    // resource description for a buffer (storing the image data in this heap just to copy to the default heap)
    D3D12_RESOURCE_STATE_GENERIC_READ, // We will copy the contents from this heap to the default heap above
    nullptr,
    IID_PPV_ARGS(&textureBufferUploadHeap)));

  textureBufferUploadHeap->SetName(L"Texture Buffer Upload Resource Heap");

  // store vertex buffer in upload heap
  D3D12_SUBRESOURCE_DATA textureData = {};
  textureData.pData = &imageData[0]; // pointer to our image data
  textureData.RowPitch = imageBytesPerRow; // size of all our triangle vertex data
  textureData.SlicePitch = imageBytesPerRow * textureDesc.Height; // also the size of our triangle vertex data

  auto commandList = m_deviceResources->GetCommandList();
  auto commandAllocator = m_deviceResources->GetCommandAllocator();

  commandList->Reset(commandAllocator, nullptr);

  // Reset the command list for the acceleration structure construction.
  UpdateSubresources(commandList, m_normalTextureBuffer.resource.Get(), textureBufferUploadHeap, 0, 0, 1, &textureData);

  // transition the texture default heap to a pixel shader resource (we will be sampling from this heap in the pixel shader to get the color of pixels)
  commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_normalTextureBuffer.resource.Get(),
                                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

  UINT descriptorIndex = AllocateDescriptor(&m_normalTextureBuffer.cpuDescriptorHandle);

  // create SRV descriptor
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Format = textureDesc.Format;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(m_normalTextureBuffer.resource.Get(), &srvDesc,
                                   m_normalTextureBuffer.cpuDescriptorHandle);

  // used to bind to the root signature
  m_normalTextureBuffer.gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
    m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, m_descriptorSize);

  // Kick off texture uploading
  m_deviceResources->ExecuteCommandList();

  // Wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope.
  m_deviceResources->WaitForGpu();
}

void D3D12RaytracingSimpleLighting::SerializeAndCreateRaytracingRootSignature(
  D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig)
{
  auto device = m_deviceResources->GetD3DDevice();
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error;

  if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
  {
    ThrowIfFailed(m_fallbackDevice->D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                  error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
    ThrowIfFailed(m_fallbackDevice->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        IID_PPV_ARGS(&(*rootSig))));
  }
  else // DirectX Raytracing
  {
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                  error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
    ThrowIfFailed(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&(*rootSig))));
  }
}

void D3D12RaytracingSimpleLighting::CreateRootSignatures()
{
  auto device = m_deviceResources->GetD3DDevice();

  // Global Root Signature
  // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
  {
    auto num_models = m_sceneLoaded->modelMap.size();
    auto num_objects = m_sceneLoaded->objects.size();
    auto num_textures = m_sceneLoaded->textureMap.size();
    auto num_materials = m_sceneLoaded->materialMap.size();

    //ensure that the models, textures and materials are not zero or else bad things will happen :)
    assert(num_models != 0);
    assert(num_objects != 0);
    assert(num_textures != 0);
    assert(num_materials != 0);

    CD3DX12_DESCRIPTOR_RANGE ranges[7]; // Perfomance TIP: Order from most frequent to least frequent.
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // 1 output texture
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, num_models, 0, 1); // array of vertices
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, num_models, 0, 2); // array of indices
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, num_objects, 0, 3); // array of infos for each object
    ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, num_materials, 0, 4); // array of materials
    ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, num_textures, 0, 5); // array of textures
    ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, num_textures, 0, 6); // array of normal textures

    CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParams::Count];
    rootParameters[GlobalRootSignatureParams::AccelerationStructureSlot].InitAsShaderResourceView(0);
    rootParameters[GlobalRootSignatureParams::SceneConstantSlot].InitAsConstantBufferView(0);
    rootParameters[GlobalRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);
    rootParameters[GlobalRootSignatureParams::VertexBuffersSlot].InitAsDescriptorTable(1, &ranges[1]);
    rootParameters[GlobalRootSignatureParams::IndexBuffersSlot].InitAsDescriptorTable(1, &ranges[2]);
    rootParameters[GlobalRootSignatureParams::InfoBuffersSlot].InitAsDescriptorTable(1, &ranges[3]);
    rootParameters[GlobalRootSignatureParams::MaterialBuffersSlot].InitAsDescriptorTable(1, &ranges[4]);
    rootParameters[GlobalRootSignatureParams::TextureSlot].InitAsDescriptorTable(1, &ranges[5]);
    rootParameters[GlobalRootSignatureParams::NormalTextureSlot].InitAsDescriptorTable(1, &ranges[6]);

    // LOOKAT
    // create a static sampler
    D3D12_STATIC_SAMPLER_DESC sampler[2] = {};
    sampler[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler[0].MipLODBias = 0;
    sampler[0].MaxAnisotropy = 0;
    sampler[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler[0].MinLOD = 0.0f;
    sampler[0].MaxLOD = 1.0f;
    sampler[0].ShaderRegister = 0;
    sampler[0].RegisterSpace = 0;
    sampler[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    memcpy(&sampler[1], &sampler[0], sizeof(D3D12_STATIC_SAMPLER_DESC));
    sampler[1].ShaderRegister = 1;

    CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters, 2, &sampler[0]);
    SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_raytracingGlobalRootSignature);
  }

  // Local Root Signature
  // This is a root signature that enables a shader to have unique arguments that come from shader tables.
  {
    CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
    rootParameters[LocalRootSignatureParams::CubeConstantSlot].InitAsConstants(SizeOfInUint32(m_cubeCB), 1);
    CD3DX12_ROOT_SIGNATURE_DESC localRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
    localRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    SerializeAndCreateRaytracingRootSignature(localRootSignatureDesc, &m_raytracingLocalRootSignature);
  }
}

// Create raytracing device and command list.
void D3D12RaytracingSimpleLighting::CreateRaytracingInterfaces()
{
  auto device = m_deviceResources->GetD3DDevice();
  auto commandList = m_deviceResources->GetCommandList();

  if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
  {
    CreateRaytracingFallbackDeviceFlags createDeviceFlags = m_forceComputeFallback
                                                              ? CreateRaytracingFallbackDeviceFlags::
                                                              ForceComputeFallback
                                                              : CreateRaytracingFallbackDeviceFlags::None;
    ThrowIfFailed(D3D12CreateRaytracingFallbackDevice(device, createDeviceFlags, 0, IID_PPV_ARGS(&m_fallbackDevice)));
    m_fallbackDevice->QueryRaytracingCommandList(commandList, IID_PPV_ARGS(&m_fallbackCommandList));
  }
  else // DirectX Raytracing
  {
    ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&m_dxrDevice)),
                  L"Couldn't get DirectX Raytracing interface for the device.\n");
    ThrowIfFailed(commandList->QueryInterface(IID_PPV_ARGS(&m_dxrCommandList)),
                  L"Couldn't get DirectX Raytracing interface for the command list.\n");
  }
}

// Local root signature and shader association
// This is a root signature that enables a shader to have unique arguments that come from shader tables.
void D3D12RaytracingSimpleLighting::CreateLocalRootSignatureSubobjects(CD3D12_STATE_OBJECT_DESC* raytracingPipeline)
{
  // Ray gen and miss shaders in this sample are not using a local root signature and thus one is not associated with them.

  // Local root signature to be used in a hit group.
  auto localRootSignature = raytracingPipeline->CreateSubobject<CD3D12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
  localRootSignature->SetRootSignature(m_raytracingLocalRootSignature.Get());
  // Define explicit shader association for the local root signature. 
  {
    auto rootSignatureAssociation = raytracingPipeline->CreateSubobject<
      CD3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
    rootSignatureAssociation->SetSubobjectToAssociate(*localRootSignature);
    rootSignatureAssociation->AddExport(c_hitGroupName);
  }
}

// Create a raytracing pipeline state object (RTPSO).
// An RTPSO represents a full set of shaders reachable by a DispatchRays() call,
// with all configuration options resolved, such as local signatures and other state.
void D3D12RaytracingSimpleLighting::CreateRaytracingPipelineStateObject()
{
  // Create 7 subobjects that combine into a RTPSO:
  // Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
  // Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
  // This simple sample utilizes default shader association except for local root signature subobject
  // which has an explicit association specified purely for demonstration purposes.
  // 1 - DXIL library
  // 1 - Triangle hit group
  // 1 - Shader config
  // 2 - Local root signature and association
  // 1 - Global root signature
  // 1 - Pipeline config
  CD3D12_STATE_OBJECT_DESC raytracingPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};


  // DXIL library
  // This contains the shaders and their entrypoints for the state object.
  // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
  auto lib = raytracingPipeline.CreateSubobject<CD3D12_DXIL_LIBRARY_SUBOBJECT>();
  D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pRaytracing, ARRAYSIZE(g_pRaytracing));
  lib->SetDXILLibrary(&libdxil);
  // Define which shader exports to surface from the library.
  // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
  // In this sample, this could be ommited for convenience since the sample uses all shaders in the library. 
  {
    lib->DefineExport(c_raygenShaderName);
    lib->DefineExport(c_closestHitShaderName);
    lib->DefineExport(c_missShaderName);
  }

  // Triangle hit group
  // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
  // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
  auto hitGroup = raytracingPipeline.CreateSubobject<CD3D12_HIT_GROUP_SUBOBJECT>();
  hitGroup->SetClosestHitShaderImport(c_closestHitShaderName);
  hitGroup->SetHitGroupExport(c_hitGroupName);
  hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

  // Shader config
  // Defines the maximum sizes in bytes for the ray payload and attribute structure.
  auto shaderConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
  UINT payloadSize = sizeof(XMFLOAT4) + sizeof(XMFLOAT3) * 2; // float4 pixelColor
  UINT attributeSize = sizeof(XMFLOAT2); // float2 barycentrics
  shaderConfig->Config(payloadSize, attributeSize);

  // Local root signature and shader association
  // This is a root signature that enables a shader to have unique arguments that come from shader tables.
  CreateLocalRootSignatureSubobjects(&raytracingPipeline);

  // Global root signature
  // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
  auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3D12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
  globalRootSignature->SetRootSignature(m_raytracingGlobalRootSignature.Get());

  // Pipeline config
  // Defines the maximum TraceRay() recursion depth.
  auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
  // PERFOMANCE TIP: Set max recursion depth as low as needed 
  // as drivers may apply optimization strategies for low recursion depths.
  UINT maxRecursionDepth = 1; // ~ primary rays only. 
  pipelineConfig->Config(maxRecursionDepth);

#if _DEBUG
  PrintStateObjectDesc(raytracingPipeline);
#endif

  // Create the state object.
  if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
  {
    ThrowIfFailed(m_fallbackDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_fallbackStateObject)),
                  L"Couldn't create DirectX Raytracing state object.\n");
  }
  else // DirectX Raytracing
  {
    ThrowIfFailed(m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)),
                  L"Couldn't create DirectX Raytracing state object.\n");
  }
}

// Create 2D output texture for raytracing.
void D3D12RaytracingSimpleLighting::CreateRaytracingOutputResource()
{
  auto device = m_deviceResources->GetD3DDevice();
  auto backbufferFormat = m_deviceResources->GetBackBufferFormat();

  // Create the output resource. The dimensions and format should match the swap-chain.
  auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(backbufferFormat, m_width, m_height, 1, 1, 1, 0,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
  ThrowIfFailed(device->CreateCommittedResource(
    &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
    IID_PPV_ARGS(&m_raytracingOutput)));
  NAME_D3D12_OBJECT(m_raytracingOutput);

  D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
  m_raytracingOutputResourceUAVDescriptorHeapIndex = AllocateDescriptor(
    &uavDescriptorHandle, m_raytracingOutputResourceUAVDescriptorHeapIndex);
  D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
  UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(m_raytracingOutput.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
  m_raytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(
    m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), m_raytracingOutputResourceUAVDescriptorHeapIndex,
    m_descriptorSize);
}

void D3D12RaytracingSimpleLighting::CreateDescriptorHeap()
{
  auto device = m_deviceResources->GetD3DDevice();

  D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
  // Allocate a heap for 5 descriptors:
  // 2 - vertex and index buffer SRVs
  // 1 - tex
  // 1 - norm tex
  // 1 - raytracing output texture SRV
  // 2 - bottom and top level acceleration structure fallback wrapped pointer UAVs
  descriptorHeapDesc.NumDescriptors = 10000;
  descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  descriptorHeapDesc.NodeMask = 0;
  device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
  NAME_D3D12_OBJECT(m_descriptorHeap);

  m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RaytracingSimpleLighting::BuildMesh(std::string path)
{
  // load mesh here
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;

  tinyobj::attrib_t attrib;
  std::string err;
  bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str());

  std::vector<Index> indices;
  std::vector<Vertex> vertices;

  if (!ret)
  {
    throw std::runtime_error("failed to load Object!");
  }
  else
  {
    // loop over shapes
    int finalIdx = 0;
    for (unsigned int s = 0; s < shapes.size(); s++)
    {
      size_t index_offset = 0;
      // loop over  faces
      for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++)
      {
        int fv = shapes[s].mesh.num_face_vertices[f];
        std::vector<float>& positions = attrib.vertices;
        std::vector<float>& normals = attrib.normals;
        std::vector<float>& texcoords = attrib.texcoords;

        // loop over vertices in face, might be > 3
        for (size_t v = 0; v < fv; v++)
        {
          tinyobj::index_t index = shapes[s].mesh.indices[v + index_offset];

          Vertex vert;
          vert.position = XMFLOAT3(positions[index.vertex_index * 3], positions[index.vertex_index * 3 + 1],
                                   positions[index.vertex_index * 3 + 2]);
          if (index.normal_index != -1)
          {
            vert.normal = XMFLOAT3(normals[index.normal_index * 3], normals[index.normal_index * 3 + 1],
                                   normals[index.normal_index * 3 + 2]);
          }

          if (index.texcoord_index != -1)
          {
            vert.texCoord = XMFLOAT2(texcoords[index.texcoord_index * 2], 1 - texcoords[index.texcoord_index * 2 + 1]);
          }
          vertices.push_back(vert);
          indices.push_back((Index)(finalIdx + index_offset + v));
        }

        index_offset += fv;
      }
      finalIdx += index_offset;
    }
    auto device = m_deviceResources->GetD3DDevice();
    Vertex* vPtr = vertices.data();
    Index* iPtr = indices.data();
    AllocateUploadBuffer(device, iPtr, indices.size() * sizeof(Index), &m_indexBuffer.resource);
    AllocateUploadBuffer(device, vPtr, vertices.size() * sizeof(Vertex), &m_vertexBuffer.resource);

    // Vertex buffer is passed to the shader along with index buffer as a descriptor table.
    // Vertex buffer descriptor must follow index buffer descriptor in the descriptor heap.
    UINT descriptorIndexIB = CreateBufferSRV(&m_indexBuffer, indices.size() * sizeof(Index) / 4, 0);
    UINT descriptorIndexVB = CreateBufferSRV(&m_vertexBuffer, vertices.size(), sizeof(Vertex));
    ThrowIfFalse(descriptorIndexVB == descriptorIndexIB + 1,
                 L"Vertex Buffer descriptor index must follow that of Index Buffer descriptor index!");
  }
}


// Build geometry used in the sample.
void D3D12RaytracingSimpleLighting::BuildGeometry()
{
  auto device = m_deviceResources->GetD3DDevice();

  // Cube indices.
  Index indices[] =
  {
    3, 1, 0,
    2, 1, 3,

    6, 4, 5,
    7, 4, 6,

    11, 9, 8,
    10, 9, 11,

    14, 12, 13,
    15, 12, 14,

    19, 17, 16,
    18, 17, 19,

    22, 20, 21,
    23, 20, 22
  };

  // Cube vertices positions and corresponding triangle normals.
  Vertex vertices[] =
  {
    {XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f)},
    {XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f)},
    {XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f)},
    {XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f)},

    {XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f)},
    {XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f)},
    {XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f)},
    {XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f)},

    {XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f)},

    {XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)},
    {XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)},

    {XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f)},
    {XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f)},
    {XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f)},
    {XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f)},

    {XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f)},
    {XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f)},
    {XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f)},
    {XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f)},
  };

  // AllocateUploadBuffer(device, indices, sizeof(indices), &m_indexBufferCube.resource);
  // AllocateUploadBuffer(device, vertices, sizeof(vertices), &m_vertexBufferCube.resource);

  // Vertex buffer is passed to the shader along with index buffer as a descriptor table.
  // Vertex buffer descriptor must follow index buffer descriptor in the descriptor heap.

  //UINT descriptorIndexIB = CreateBufferSRV(&m_indexBufferCube, 36 * sizeof(Index) / 4, 0);
  //UINT descriptorIndexVB = CreateBufferSRV(&m_vertexBufferCube, 6 , sizeof(vertices[0]));
  //ThrowIfFalse(descriptorIndexVB == descriptorIndexIB + 1, L"Vertex Buffer descriptor index must follow that of Index Buffer descriptor index!");
  //ThrowIfFalse(descriptorIndexVB == descriptorIndexIB + 1, L"Vertex Buffer descriptor index must follow that of Index Buffer descriptor index!");
}

// Build acceleration structures needed for raytracing.
void D3D12RaytracingSimpleLighting::BuildAccelerationStructures()
{
  auto device = m_deviceResources->GetD3DDevice();
  auto commandList = m_deviceResources->GetCommandList();
  auto commandQueue = m_deviceResources->GetCommandQueue();
  auto commandAllocator = m_deviceResources->GetCommandAllocator();

  // Reset the command list for the acceleration structure construction.
  commandList->Reset(commandAllocator, nullptr);

  // What API is used
  bool is_fallback = m_raytracingAPI == RaytracingAPI::FallbackLayer;

  // Build bottom levels
  for (auto& model_pair : m_sceneLoaded->modelMap)
  {
    //int id = model_pair.first;
    ModelLoading::Model& model = model_pair.second;
    model.GetGeomDesc();
    model.GetBottomLevelBuildDesc();
    model.GetPreBuild(is_fallback, m_fallbackDevice, m_dxrDevice);
    model.GetBottomLevelScratchAS(is_fallback, device, m_fallbackDevice, m_dxrDevice);
    model.GetBottomAS(is_fallback, device, m_fallbackDevice, m_dxrDevice);

    //m_sceneLoaded->modelMap[id] = model;
  }

  //GAME
  if (is_game)
  {
#define NUMBER_OF_PREALLOCATED_FACES (10000)
    for (int i = 1; i < NUMBER_OF_PREALLOCATED_FACES; i++)
    {
      ModelLoading::SceneObject first_object = m_sceneLoaded->objects[0];
      first_object.id = i;
      m_sceneLoaded->objects.push_back(first_object);
    }
  }


  // Build top level
  m_sceneLoaded->GetTopLevelDesc();
  m_sceneLoaded->GetTopLevelPrebuildInfo(is_fallback, m_fallbackDevice, m_dxrDevice);
  m_sceneLoaded->GetTopLevelScratchAS(is_fallback, device, m_fallbackDevice, m_dxrDevice);
  m_sceneLoaded->GetTopAS(is_fallback, device, m_fallbackDevice, m_dxrDevice);
  m_sceneLoaded->GetInstanceDescriptors(is_fallback, m_fallbackDevice, m_dxrDevice);

  // Finalize the allocation
  m_fallbackTopLevelAccelerationStructurePointer = m_sceneLoaded->GetWrappedGPUPointer(
    is_fallback, m_fallbackDevice, m_dxrDevice);
  m_sceneLoaded->FinalizeAS();

  // Actually build the AS (executing command lists)
  m_sceneLoaded->BuildAllAS(is_fallback, m_fallbackDevice, m_dxrDevice, m_fallbackCommandList, m_dxrCommandList);

  //GAME
  if (is_game)
  {
    mini_face_manager = std::make_unique<MiniFaceManager>();
    mini_face_manager->SetScene(m_sceneLoaded);
    mini_terrian.program_state = this;
    generateMoreChunks();

    // m_eye = {0.0f, 128.0f, -20.0f, 1.0f};
    // m_at = {0.0f, 128.0f, 1.0f, 1.0f};    
    m_eye = {0.0f, 0.0f, 0.0f, 1.0f};
    m_at = {0.0f, 0.0f, -1.0f, 1.0f};  
    
    chunk_generation_thread = std::thread([this]()
    {
      while (!quit_generation)
      {
        {
          std::unique_lock<std::mutex> lock(chunk_mutex);
          chunk_cv.wait(lock, [this]{ return !start_generation; });
        }
        std::unique_lock<std::mutex> lock(chunk_mutex);

        float elapsedTime = static_cast<float>(m_timer.GetElapsedSeconds());
        float secondsToRotateAround = 8.0f;
        float angleToRotateBy = -360.0f * (elapsedTime / secondsToRotateAround);
        
        mini_face_manager->Reset();
        
        MiniFace* mini_face = mini_face_manager->AllocateMiniFace();
        if (mini_face != nullptr)
        {
          //glm::mat4 mat;
          //mat = glm::rotate(mat, glm::radians(angleToRotateBy), glm::vec3(0, 1, 0));
          //mat = glm::translate(mat, mini_block->GetTranslation());
    
          glm::vec3 block_rotation = mini_face->GetRotation();
          block_rotation.y += glm::radians(angleToRotateBy) * 100;
          mini_face->SetRotation(block_rotation);
          // mini_block->SetTranslation(mini_block->GetTranslation() +
          //                             glm::vec3(0.01f, 0.0, 0.0));
    
        }
        //recreateChunkVBO();
        chunk_cv.wait(lock, [this]{ return start_generation; });

        chunk_promise.set_value(false);
      }
    });
  }
}

// Build shader tables.
// This encapsulates all shader records - shaders and the arguments for their local root signatures.
void D3D12RaytracingSimpleLighting::BuildShaderTables()
{
  auto device = m_deviceResources->GetD3DDevice();

  void* rayGenShaderIdentifier;
  void* missShaderIdentifier;
  void* hitGroupShaderIdentifier;

  auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
  {
    rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_raygenShaderName);
    missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_missShaderName);
    hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_hitGroupName);
  };

  // Get shader identifiers.
  UINT shaderIdentifierSize;
  if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
  {
    GetShaderIdentifiers(m_fallbackStateObject.Get());
    shaderIdentifierSize = m_fallbackDevice->GetShaderIdentifierSize();
  }
  else // DirectX Raytracing
  {
    ComPtr<ID3D12StateObjectPropertiesPrototype> stateObjectProperties;
    ThrowIfFailed(m_dxrStateObject.As(&stateObjectProperties));
    GetShaderIdentifiers(stateObjectProperties.Get());
    shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
  }

  // Ray gen shader table
  {
    UINT numShaderRecords = 1;
    UINT shaderRecordSize = shaderIdentifierSize;
    ShaderTable rayGenShaderTable(device, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
    rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
    m_rayGenShaderTable = rayGenShaderTable.GetResource();
  }

  // Miss shader table
  {
    UINT numShaderRecords = 1;
    UINT shaderRecordSize = shaderIdentifierSize;
    ShaderTable missShaderTable(device, numShaderRecords, shaderRecordSize, L"MissShaderTable");
    missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
    m_missShaderTable = missShaderTable.GetResource();
  }

  // Hit group shader table
  {
    struct RootArguments
    {
      CubeConstantBuffer cb;
    } rootArguments;
    rootArguments.cb = m_cubeCB;

    UINT numShaderRecords = 1;
    UINT shaderRecordSize = shaderIdentifierSize + sizeof(rootArguments);
    ShaderTable hitGroupShaderTable(device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
    hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments,
                                               sizeof(rootArguments)));
    m_hitGroupShaderTable = hitGroupShaderTable.GetResource();
  }
}

void D3D12RaytracingSimpleLighting::SelectRaytracingAPI(RaytracingAPI type)
{
  if (type == RaytracingAPI::FallbackLayer)
  {
    m_raytracingAPI = type;
  }
  else // DirectX Raytracing
  {
    if (m_isDxrSupported)
    {
      m_raytracingAPI = type;
    }
    else
    {
      OutputDebugString(L"Invalid selection - DXR is not available.\n");
    }
  }
}

// Update frame-based values.
void D3D12RaytracingSimpleLighting::OnUpdate()
{
  m_timer.Tick();
  CalculateFrameStats();
  float elapsedTime = static_cast<float>(m_timer.GetElapsedSeconds());
  auto frameIndex = m_deviceResources->GetCurrentFrameIndex();
  auto prevFrameIndex = m_deviceResources->GetPreviousFrameIndex();

  UpdateCameraMatrices();

  // Rotate the second light around Y axis.
  {
    float secondsToRotateAround = 8.0f;
    float angleToRotateBy = -360.0f * (elapsedTime / secondsToRotateAround);
    XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(angleToRotateBy));
    const XMVECTOR& prevLightPosition = m_sceneCB[prevFrameIndex].lightPosition;
    m_sceneCB[frameIndex].lightPosition = XMVector3Transform(prevLightPosition, rotate);
  }

  {
    if (m_camChanged)
    {
      m_sceneCB[frameIndex].iteration = 1;
      m_camChanged = false;
    }
    else
    {
      if (m_sceneCB[frameIndex].iteration < c_maxIteration)
      {
        m_sceneCB[frameIndex].iteration += 1;
      }
    }
  }
}


// Parse supplied command line args.
void D3D12RaytracingSimpleLighting::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
  DXSample::ParseCommandLineArgs(argv, argc);

  if (argc > 1)
  {
    if (_wcsnicmp(argv[1], L"-FL", wcslen(argv[1])) == 0)
    {
      m_forceComputeFallback = true;
      m_raytracingAPI = RaytracingAPI::FallbackLayer;
    }
    else if (_wcsnicmp(argv[1], L"-DXR", wcslen(argv[1])) == 0)
    {
      m_raytracingAPI = RaytracingAPI::DirectXRaytracing;
    }
  }
}

void D3D12RaytracingSimpleLighting::DoRaytracing()
{
  auto commandList = m_deviceResources->GetCommandList();
  auto commandAllocator = m_deviceResources->GetCommandAllocator();
  auto frameIndex = m_deviceResources->GetCurrentFrameIndex();

  auto DispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
  {
    // Since each shader table has only one shader record, the stride is same as the size.
    dispatchDesc->HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc->HitGroupTable.SizeInBytes = m_hitGroupShaderTable->GetDesc().Width;
    dispatchDesc->HitGroupTable.StrideInBytes = dispatchDesc->HitGroupTable.SizeInBytes;
    dispatchDesc->MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc->MissShaderTable.SizeInBytes = m_missShaderTable->GetDesc().Width;
    dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;
    dispatchDesc->RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable->GetDesc().Width;
    dispatchDesc->Width = m_width;
    dispatchDesc->Height = m_height;
    dispatchDesc->Depth = 1;
    commandList->SetPipelineState1(stateObject);
    commandList->DispatchRays(dispatchDesc);
  };


  auto SetCommonPipelineState = [&](auto* descriptorSetCommandList)
  {
    ModelLoading::SceneObject& objectInScene = m_sceneLoaded->objects[0];
    ModelLoading::Texture& textures = m_sceneLoaded->textureMap[0];
    ModelLoading::MaterialResource& material = m_sceneLoaded->materialMap[0];
    descriptorSetCommandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
    // Set index and successive vertex buffer decriptor tables
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::VertexBuffersSlot,
                                               objectInScene.model->vertices.gpuDescriptorHandle);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::IndexBuffersSlot,
                                               objectInScene.model->indices.gpuDescriptorHandle);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::InfoBuffersSlot,
                                               objectInScene.info_resource.d3d12_resource.gpuDescriptorHandle);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot,
                                               m_raytracingOutputResourceUAVGpuDescriptor);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::TextureSlot,
                                               textures.texBuffer.gpuDescriptorHandle);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::NormalTextureSlot,
                                               textures.texBuffer.gpuDescriptorHandle);
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::MaterialBuffersSlot,
                                               material.d3d12_material_resource.gpuDescriptorHandle);
  };

  commandList->SetComputeRootSignature(m_raytracingGlobalRootSignature.Get());

  // Copy the updated scene constant buffer to GPU.
  memcpy(&m_mappedConstantData[frameIndex].constants, &m_sceneCB[frameIndex], sizeof(m_sceneCB[frameIndex]));
  auto cbGpuAddress = m_perFrameConstants->GetGPUVirtualAddress() + frameIndex * sizeof(m_mappedConstantData[0]);
  commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::SceneConstantSlot, cbGpuAddress);

  // Bind the heaps, acceleration structure and dispatch rays.ExecuteCommandLists
  D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
  if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
  {
    SetCommonPipelineState(m_fallbackCommandList.Get());
    m_fallbackCommandList->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot,
                                                            m_fallbackTopLevelAccelerationStructurePointer);
    DispatchRays(m_fallbackCommandList.Get(), m_fallbackStateObject.Get(), &dispatchDesc);
  }
  else // DirectX Raytracing
  {
    SetCommonPipelineState(commandList);
    commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::AccelerationStructureSlot,
                                                  m_topLevelAccelerationStructure->GetGPUVirtualAddress());
    DispatchRays(m_dxrCommandList.Get(), m_dxrStateObject.Get(), &dispatchDesc);
  }

}

// Update the application state with the new resolution.
void D3D12RaytracingSimpleLighting::UpdateForSizeChange(UINT width, UINT height)
{
  DXSample::UpdateForSizeChange(width, height);
}

// Copy the raytracing output to the backbuffer.
void D3D12RaytracingSimpleLighting::CopyRaytracingOutputToBackbuffer()
{
  auto commandList = m_deviceResources->GetCommandList();
  auto renderTarget = m_deviceResources->GetRenderTarget();

  D3D12_RESOURCE_BARRIER preCopyBarriers[2];
  preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                            D3D12_RESOURCE_STATE_COPY_DEST);
  preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(),
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

  commandList->CopyResource(renderTarget, m_raytracingOutput.Get());

  D3D12_RESOURCE_BARRIER postCopyBarriers[2];
  postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_PRESENT);
  postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
}

// Create resources that are dependent on the size of the main window.
void D3D12RaytracingSimpleLighting::CreateWindowSizeDependentResources()
{
  CreateRaytracingOutputResource();
  UpdateCameraMatrices();
}

// Release resources that are dependent on the size of the main window.
void D3D12RaytracingSimpleLighting::ReleaseWindowSizeDependentResources()
{
  m_raytracingOutput.Reset();
}

// Release all resources that depend on the device.
void D3D12RaytracingSimpleLighting::ReleaseDeviceDependentResources()
{
  m_fallbackDevice.Reset();
  m_fallbackCommandList.Reset();
  m_fallbackStateObject.Reset();
  m_raytracingGlobalRootSignature.Reset();
  m_raytracingLocalRootSignature.Reset();

  m_dxrDevice.Reset();
  m_dxrCommandList.Reset();
  m_dxrStateObject.Reset();

  m_descriptorHeap.Reset();
  m_descriptorsAllocated = 0;
  m_raytracingOutputResourceUAVDescriptorHeapIndex = UINT_MAX;
  m_indexBuffer.resource.Reset();
  m_vertexBuffer.resource.Reset();
  m_textureBuffer.resource.Reset();
  m_normalTextureBuffer.resource.Reset();
  m_perFrameConstants.Reset();
  m_rayGenShaderTable.Reset();
  m_missShaderTable.Reset();
  m_hitGroupShaderTable.Reset();

  m_bottomLevelAccelerationStructure.Reset();
  m_topLevelAccelerationStructure.Reset();

  //GAME
  if (is_game)
  {
    quit_generation = true;
    {
      std::lock_guard<std::mutex> lock(chunk_mutex);
      start_generation = true;
    }
    chunk_cv.notify_all();

    start_generation = chunk_promise.get_future().get();
    chunk_promise = std::promise<bool>();

    chunk_generation_thread.join();
  }
}

void D3D12RaytracingSimpleLighting::RecreateD3D()
{
  // Give GPU a chance to finish its execution in progress.
  try
  {
    m_deviceResources->WaitForGpu();
  }
  catch (HrException&)
  {
    // Do nothing, currently attached adapter is unresponsive.
  }
  m_deviceResources->HandleDeviceLost();
}

// Render the scene.
void D3D12RaytracingSimpleLighting::OnRender()
{
  if (!m_deviceResources->IsWindowVisible())
  {
    return;
  }

  //GAME
  if (is_game)
  {
    m_deviceResources->WaitForGpu();
  }

  m_deviceResources->Prepare();
  
  //GAME
  if (is_game)
  {
    {
      std::lock_guard<std::mutex> lock(chunk_mutex);
      start_generation = true;
    }
    chunk_cv.notify_all();

    start_generation = chunk_promise.get_future().get();
    chunk_promise = std::promise<bool>();

    m_sceneLoaded->UpdateTopLevelAS(m_raytracingAPI == RaytracingAPI::FallbackLayer, m_fallbackDevice, m_dxrDevice, m_fallbackCommandList, m_dxrCommandList);    

    auto frame_index = m_deviceResources->GetCurrentFrameIndex();
    m_sceneCB[frame_index].is_raytracing = 1;

    chunk_cv.notify_all();
  }

  DoRaytracing();
  CopyRaytracingOutputToBackbuffer();

  m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12RaytracingSimpleLighting::OnDestroy()
{
  // Let GPU finish before releasing D3D resources.
  m_deviceResources->WaitForGpu();
  OnDeviceLost();
}

// Release all device dependent resouces when a device is lost.
void D3D12RaytracingSimpleLighting::OnDeviceLost()
{
  ReleaseWindowSizeDependentResources();
  ReleaseDeviceDependentResources();
}

// Create all device dependent resources when a device is restored.
void D3D12RaytracingSimpleLighting::OnDeviceRestored()
{
  CreateDeviceDependentResources();
  CreateWindowSizeDependentResources();
}

// Compute the average frames per second and million rays per second.
void D3D12RaytracingSimpleLighting::CalculateFrameStats()
{
  static int frameCnt = 0;
  static double elapsedTime = 0.0f;
  double totalTime = m_timer.GetTotalSeconds();
  frameCnt++;

  // Compute averages over one second period.
  if ((totalTime - elapsedTime) >= 1.0f)
  {
    float diff = static_cast<float>(totalTime - elapsedTime);
    float fps = static_cast<float>(frameCnt) / diff; // Normalize to an exact second.

    frameCnt = 0;
    elapsedTime = totalTime;

    float MRaysPerSecond = (m_width * m_height * fps) / static_cast<float>(1e6);

    wstringstream windowText;

    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
      if (m_fallbackDevice->UsingRaytracingDriver())
      {
        windowText << L"(FL-DXR)";
      }
      else
      {
        windowText << L"(FL)";
      }
    }
    else
    {
      windowText << L"(DXR)";
    }
    windowText << setprecision(2) << fixed
      << L"    fps: " << fps << L"     ~Million Primary Rays/s: " << MRaysPerSecond
      << L"    GPU[" << m_deviceResources->GetAdapterID() << L"]: " << m_deviceResources->GetAdapterDescription();
    SetCustomWindowText(windowText.str().c_str());
  }
}

// Handle OnSizeChanged message event.
void D3D12RaytracingSimpleLighting::OnSizeChanged(UINT width, UINT height, bool minimized)
{
  if (!m_deviceResources->WindowSizeChanged(width, height, minimized))
  {
    return;
  }

  UpdateForSizeChange(width, height);

  ReleaseWindowSizeDependentResources();
  CreateWindowSizeDependentResources();
}

// Create a wrapped pointer for the Fallback Layer path.
WRAPPED_GPU_POINTER D3D12RaytracingSimpleLighting::CreateFallbackWrappedPointer(
  ID3D12Resource* resource, UINT bufferNumElements)
{
  auto device = m_deviceResources->GetD3DDevice();

  D3D12_UNORDERED_ACCESS_VIEW_DESC rawBufferUavDesc = {};
  rawBufferUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  rawBufferUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  rawBufferUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
  rawBufferUavDesc.Buffer.NumElements = bufferNumElements;

  D3D12_CPU_DESCRIPTOR_HANDLE bottomLevelDescriptor;

  // Only compute fallback requires a valid descriptor index when creating a wrapped pointer.
  UINT descriptorHeapIndex = 0;
  if (!m_fallbackDevice->UsingRaytracingDriver())
  {
    descriptorHeapIndex = AllocateDescriptor(&bottomLevelDescriptor);
    device->CreateUnorderedAccessView(resource, nullptr, &rawBufferUavDesc, bottomLevelDescriptor);
  }
  return m_fallbackDevice->GetWrappedPointerSimple(descriptorHeapIndex, resource->GetGPUVirtualAddress());
}

// Allocate a descriptor and return its index. 
// If the passed descriptorIndexToUse is valid, it will be used instead of allocating a new one.
UINT D3D12RaytracingSimpleLighting::AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor,
                                                       UINT descriptorIndexToUse)
{
  auto descriptorHeapCpuBase = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
  if (descriptorIndexToUse >= m_descriptorHeap->GetDesc().NumDescriptors)
  {
    descriptorIndexToUse = m_descriptorsAllocated++;
  }
  *cpuDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorHeapCpuBase, descriptorIndexToUse, m_descriptorSize);
  return descriptorIndexToUse;
}

// Create SRV for a buffer.
UINT D3D12RaytracingSimpleLighting::CreateBufferSRV(D3DBuffer* buffer, UINT numElements, UINT elementSize)
{
  auto device = m_deviceResources->GetD3DDevice();

  // SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Buffer.NumElements = numElements;
  if (elementSize == 0)
  {
    srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srvDesc.Buffer.StructureByteStride = 0;
  }
  else
  {
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srvDesc.Buffer.StructureByteStride = elementSize;
  }
  UINT descriptorIndex = AllocateDescriptor(&buffer->cpuDescriptorHandle);
  device->CreateShaderResourceView(buffer->resource.Get(), &srvDesc, buffer->cpuDescriptorHandle);
  buffer->gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(),
                                                              descriptorIndex, m_descriptorSize);
  return descriptorIndex;
}

void D3D12RaytracingSimpleLighting::OnKeyDown(UINT8 key)
{
  // Store previous values.
  RaytracingAPI previousRaytracingAPI = m_raytracingAPI;
  bool previousForceComputeFallback = m_forceComputeFallback;

  switch (key)
  {
  case VK_NUMPAD1:
  case '1': // Fallback Layer
    m_forceComputeFallback = false;
    SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
    break;
  case VK_NUMPAD2:
  case '2': // Fallback Layer + force compute path
    m_forceComputeFallback = true;
    SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
    break;
  case VK_NUMPAD3:
  case '3': // DirectX Raytracing
    SelectRaytracingAPI(RaytracingAPI::DirectXRaytracing);
    break;
  case 0x44: // D
    {
      XMFLOAT4 right;
      XMVector4Normalize(m_right);
      XMStoreFloat4(&right, m_right);
      XMMATRIX translate = XMMatrixTranslation(-right.x * c_movementAmountFactor, -right.y * c_movementAmountFactor,
                                               -right.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case 0x41: // A
    {
      XMFLOAT4 right;
      XMVector4Normalize(m_right);
      XMStoreFloat4(&right, m_right);
      XMMATRIX translate = XMMatrixTranslation(right.x * c_movementAmountFactor, right.y * c_movementAmountFactor,
                                               right.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case 0x53: // S
    {
      XMFLOAT4 up;
      XMVector4Normalize(m_up);
      XMStoreFloat4(&up, m_up);
      XMMATRIX translate = XMMatrixTranslation(up.x * c_movementAmountFactor, up.y * c_movementAmountFactor,
                                               up.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case 0x57: // W
    {
      XMFLOAT4 up;
      XMVector4Normalize(m_up);
      XMStoreFloat4(&up, m_up);
      XMMATRIX translate = XMMatrixTranslation(-up.x * c_movementAmountFactor, -up.y * c_movementAmountFactor,
                                               -up.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case 0x45: // E
    {
      XMFLOAT4 forward;
      XMVector4Normalize(m_forward);
      XMStoreFloat4(&forward, m_forward);
      XMMATRIX translate = XMMatrixTranslation(forward.x * c_movementAmountFactor, forward.y * c_movementAmountFactor,
                                               forward.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case 0x51: // Q
    {
      XMFLOAT4 forward;
      XMVector4Normalize(m_forward);
      XMStoreFloat4(&forward, m_forward);
      XMMATRIX translate = XMMatrixTranslation(-forward.x * c_movementAmountFactor, -forward.y * c_movementAmountFactor,
                                               -forward.z * c_movementAmountFactor);

      m_eye = XMVector3Transform(m_eye, translate);
      m_at = XMVector3Transform(m_at, translate);
      m_camChanged = true;
      break;
    }

  case VK_UP: // Up arrow
    {
      XMMATRIX rotate = XMMatrixRotationAxis(m_right, XMConvertToRadians(-c_rotateDegrees));
      m_eye = XMVector3Transform(m_eye, rotate);
      m_up = XMVector3Transform(m_up, rotate);
      m_forward = XMVector3Transform(m_forward, rotate);
      m_at = XMVector3Transform(m_at, rotate);
      m_camChanged = true;
      break;
    }

  case VK_DOWN: // Down arrow
    {
      XMMATRIX rotate = XMMatrixRotationAxis(m_right, XMConvertToRadians(c_rotateDegrees));
      m_eye = XMVector3Transform(m_eye, rotate);
      m_up = XMVector3Transform(m_up, rotate);
      m_forward = XMVector3Transform(m_forward, rotate);
      m_at = XMVector3Transform(m_at, rotate);
      m_camChanged = true;
      break;
    }

  case VK_RIGHT: // Right arrow
    {
      XMMATRIX rotate = XMMatrixRotationAxis(m_up, XMConvertToRadians(c_rotateDegrees));
      m_eye = XMVector3Transform(m_eye, rotate);
      m_right = XMVector3Transform(m_right, rotate);
      m_forward = XMVector3Transform(m_forward, rotate);
      m_at = XMVector3Transform(m_at, rotate);
      m_camChanged = true;
      break;
    }

  case VK_LEFT: // Left arrow
    {
      XMMATRIX rotate = XMMatrixRotationAxis(m_up, XMConvertToRadians(-c_rotateDegrees));
      m_eye = XMVector3Transform(m_eye, rotate);
      m_right = XMVector3Transform(m_right, rotate);
      m_forward = XMVector3Transform(m_forward, rotate);
      m_at = XMVector3Transform(m_at, rotate);
      m_camChanged = true;
      break;
    }
  }

  if (m_raytracingAPI != previousRaytracingAPI ||
    m_forceComputeFallback != previousForceComputeFallback)
  {
    // Raytracing API selection changed, recreate everything.
    RecreateD3D();
  }
}

void D3D12RaytracingSimpleLighting::generateMoreChunks() {
    /* Cast the player's position to bottom left corner of current block */
    auto frameIndex = m_deviceResources->GetCurrentFrameIndex();
    XMFLOAT3 myPos;
    XMStoreFloat3(&myPos, m_sceneCB[frameIndex].cameraPosition);
    int xPos = myPos.x < 0 ? ceil(myPos.x) : floor(myPos.x);
    int yPos = myPos.y < 0 ? ceil(myPos.y) : floor(myPos.y);
    int zPos = myPos.z < 0 ? ceil(myPos.z) : floor(myPos.z);
    glm::ivec3 myIntPos(xPos, yPos, zPos);

    /* Get the chunk that the player is in */
    glm::ivec3 chunk = MiniTerrain::ConvertWorldToChunk(myIntPos);
#define RENDER_DISTANCE (1)

    /* For the 2D x-z area of chunks around the player */
    for (int x = -RENDER_DISTANCE; x <= RENDER_DISTANCE; x++) {
        for (int z = -RENDER_DISTANCE; z <= RENDER_DISTANCE; z++) {
            /* If a chunk doesn't exist within RENDER_DISTANCE of player, populate it*/
            uint64_t chunkKey = MiniTerrain::ConvertChunkToKey(chunk + glm::ivec3(x,0,z));
            if (!mini_terrian.chunkExists(chunkKey)) {
                Chunk* newChunk = new Chunk(chunkKey, &mini_terrian);
                newChunk->populateChunk();
                chunksToBeDrawn.push_back(newChunk);
            }
        }
    }
}

void D3D12RaytracingSimpleLighting::recreateChunkVBO() {
    /* Make a deep copy of the shared chunk list. Copy what has been populated so far. */
    std::vector<Chunk*> currentListState = chunksToBeDrawn;

    /* Create the VBOs for the new chunk and redraw the old border chunks*/
    if (!currentListState.empty()) {
        for (int i = 0; i < currentListState.size(); i++) {
            Chunk* c = currentListState[i];
            glm::ivec3 chunkCoords = MiniTerrain::ConvertKeyToChunk(c->key);
            std::vector<uint64_t> neighborChunkKeys;
            neighborChunkKeys.push_back(MiniTerrain::ConvertChunkToKey(chunkCoords + glm::ivec3(1,0,0)));
            neighborChunkKeys.push_back(MiniTerrain::ConvertChunkToKey(chunkCoords + glm::ivec3(0,0,1)));
            neighborChunkKeys.push_back(MiniTerrain::ConvertChunkToKey(chunkCoords + glm::ivec3(0,0,-1)));
            neighborChunkKeys.push_back(MiniTerrain::ConvertChunkToKey(chunkCoords + glm::ivec3(-1,0,0)));

            /* Recreate old border chunks' VBOs */
            for (uint64_t key : neighborChunkKeys) {
                if (mini_terrian.chunkExists(key)) {
                    Chunk* neighbor = mini_terrian.chunkMap[key];
                    auto found_neighbor = std::find_if(
                        std::begin(currentListState), std::end(currentListState), [&neighbor](const auto& chunk)
                    {
                      return neighbor == chunk;
                    });
                    if (found_neighbor == std::end(currentListState)) {
                        neighbor->destroy();
                        neighbor->create(mini_face_manager.get()); //recreates hull
                    }
                }
            }
            c->create(mini_face_manager.get());
            c->toDraw = true;
        }
    }
}
