
#include <assert.h>
#include <chrono>
#include "plugin/deformableConvPlugin/deformableConvPlugin.h"
#include "common.h"
#include "amirCommon.h"
#include "serialize.hpp"
#include "deform_conv_cuda.h"

namespace amirstan
{
namespace plugin
{

namespace
{
static const char *DCN_VERSION{"1"};
static const char *DCN_NAME{"DeformableConvPluginDynamic"};
} // namespace

PluginFieldCollection DeformableConvPluginDynamicCreator::mFC{};
std::vector<PluginField> DeformableConvPluginDynamicCreator::mPluginAttributes({PluginField("out_dims"),
                                                                                PluginField("type_id"),
                                                                                PluginField("kernel_size"),
                                                                                PluginField("W"),
                                                                                PluginField("stride"),
                                                                                PluginField("padding"),
                                                                                PluginField("dilation"),
                                                                                PluginField("deformable_group"),
                                                                                PluginField("group")});

DeformableConvPluginDynamic::DeformableConvPluginDynamic(
    const std::string &name, const nvinfer1::DataType &type,
    const int outDim, const nvinfer1::Dims &kernelSize,
    const nvinfer1::Weights &W)
    : mLayerName(name),
      mType(type),
      mOutDim(outDim),
      mKernelSize(kernelSize),
      mW(W),
      mNumParamsW(W.count)
{
    // init params
    mStride = nvinfer1::Dims{2, {1, 1}};
    mPadding = nvinfer1::Dims{2, {0, 0}};
    mDilation = nvinfer1::Dims{2, {1, 1}};
    mDeformableGroup = 1;
    mGroup = 1;

    size_t wordSize = samplesCommon::getElementSize(mType);
    mWhost = std::shared_ptr<char>(new char[mNumParamsW * wordSize]);
    memcpy((void *)mWhost.get(), mW.values, mW.count * wordSize);
    mW.values = mWhost.get();
    mWdev = nullptr;
    initialize();
}

DeformableConvPluginDynamic::DeformableConvPluginDynamic(const std::string name, const void *data, size_t length)
    : mLayerName(name)
{
    deserialize_value(&data, &length, &mType);
    deserialize_value(&data, &length, &mOutDim);
    deserialize_value(&data, &length, &mKernelSize);
    deserialize_value(&data, &length, &mNumParamsW);

    deserialize_value(&data, &length, &mStride);
    deserialize_value(&data, &length, &mPadding);
    deserialize_value(&data, &length, &mDilation);
    deserialize_value(&data, &length, &mDeformableGroup);
    deserialize_value(&data, &length, &mGroup);

    size_t wordSize = samplesCommon::getElementSize(mType);

    const char *d = static_cast<const char *>(data);
    
    // mWdev = deserToDev<char>(d, mNumParamsW * wordSize);
    char* w_data = deserToHost<char>(d, mNumParamsW * wordSize);
    mWhost = std::shared_ptr<char>((char *)w_data);
    mW.values = mWhost.get();

    mW.count = mNumParamsW;
    mW.type = mType;
    mWdev = nullptr;
    initialize();
}

void DeformableConvPluginDynamic::setStrideNd(nvinfer1::Dims stride)
{
    mStride = stride;
}

nvinfer1::Dims DeformableConvPluginDynamic::getStrideNd() const
{
    return mStride;
}

void DeformableConvPluginDynamic::setPaddingNd(nvinfer1::Dims padding)
{
    mPadding = padding;
}

nvinfer1::Dims DeformableConvPluginDynamic::getPaddingNd() const
{
    return mPadding;
}

void DeformableConvPluginDynamic::setDilationNd(nvinfer1::Dims dilation)
{
    mDilation = dilation;
}

nvinfer1::Dims DeformableConvPluginDynamic::getDilationNd() const
{
    return mDilation;
}

void DeformableConvPluginDynamic::setDeformableGroup(int deformableGroup)
{
    mDeformableGroup = deformableGroup;
}

int DeformableConvPluginDynamic::getDeformableGroup()
{
    return mDeformableGroup;
}

void DeformableConvPluginDynamic::setGroup(int group)
{
    mGroup = group;
}

int DeformableConvPluginDynamic::getGroup()
{
    return mGroup;
}

nvinfer1::IPluginV2DynamicExt *DeformableConvPluginDynamic::clone() const
{
    DeformableConvPluginDynamic *plugin = new DeformableConvPluginDynamic(mLayerName,
                                                                          mType,
                                                                          mOutDim,
                                                                          mKernelSize,
                                                                          mW);
    plugin->setPluginNamespace(getPluginNamespace());
    plugin->setStrideNd(mStride);
    plugin->setPaddingNd(mPadding);
    plugin->setDilationNd(mDilation);
    plugin->setDeformableGroup(mDeformableGroup);
    plugin->setGroup(mGroup);

    return plugin;
}

inline int convDim(int input_size, int kernel_size, int dilation, int padding, int stride)
{
    return int((input_size - dilation * (kernel_size - 1) + 2 * padding - 1) / float(stride) + 1);
}

nvinfer1::DimsExprs DeformableConvPluginDynamic::getOutputDimensions(
    int outputIndex, const nvinfer1::DimsExprs *inputs, int nbInputs, nvinfer1::IExprBuilder &exprBuilder)
{
    assert(nbInputs == 2);
    assert(inputs[0].nbDims == 4);
    assert(inputs[1].nbDims == 4);
    assert(outputIndex == 0);

    nvinfer1::DimsExprs ret;
    ret.nbDims = 4;
    ret.d[0] = inputs[0].d[0];
    ret.d[1] = exprBuilder.constant(mOutDim);

    ret.d[2] = inputs[1].d[2];
    ret.d[3] = inputs[1].d[3];

    return ret;
}

bool DeformableConvPluginDynamic::supportsFormatCombination(int pos, const nvinfer1::PluginTensorDesc *inOut, int nbInputs, int nbOutputs)
{
    assert(0 <= pos && pos < 3);
    const auto *in = inOut;
    const auto *out = inOut + nbInputs;
    switch (pos)
    {
    case 0:
        return in[0].type== DataType::kFLOAT && in[0].format == nvinfer1::TensorFormat::kLINEAR;
    case 1:
        return in[1].type == in[0].type &&
               in[1].format == nvinfer1::TensorFormat::kLINEAR;
    case 2:
        return out[0].type == in[0].type &&
               out[0].format == nvinfer1::TensorFormat::kLINEAR;
    }
}

void DeformableConvPluginDynamic::configurePlugin(
    const nvinfer1::DynamicPluginTensorDesc *inputs, int nbInputs, const nvinfer1::DynamicPluginTensorDesc *outputs, int nbOutputs)
{
    // Validate input arguments
    assert(nbOutputs == 1);
    assert(nbInputs == 2);
    assert(mType == inputs[0].desc.type);
    // const auto &inDims0 = inputs[0].desc.dims;
}

size_t DeformableConvPluginDynamic::getWorkspaceSize(
    const nvinfer1::PluginTensorDesc *inputs, int nbInputs, const nvinfer1::PluginTensorDesc *outputs, int nbOutputs) const
{

    int sizeof_dtype = samplesCommon::getElementSize(mType);

    int batch_size = inputs[0].dims.d[0];
    int nInputPlane = inputs[0].dims.d[1];
    int inputHeight = inputs[0].dims.d[2];
    int inputWidth = inputs[0].dims.d[3];

    int nOutputPlane = outputs[0].dims.d[1];
    int outputHeight = outputs[0].dims.d[2];
    int outputWidth = outputs[0].dims.d[3];

    int kW = mKernelSize.d[0];
    int kH = mKernelSize.d[1];
    int im2col_step = std::min(int(batch_size), 64);

    size_t col_size = nInputPlane * kW * kH * im2col_step * outputHeight * outputWidth * sizeof_dtype;

    size_t out_size = 0;
    if (im2col_step != 1)
        out_size = batch_size * nOutputPlane * outputHeight * outputWidth * sizeof_dtype;

    return col_size + out_size + 100 * sizeof(float);
}

int DeformableConvPluginDynamic::enqueue(const nvinfer1::PluginTensorDesc *inputDesc, const nvinfer1::PluginTensorDesc *outputDesc,
                                         const void *const *inputs, void *const *outputs, void *workSpace, cudaStream_t stream)
{
    if (m_cuda_stream != stream)
    {
        cublasSetStream(m_cublas_handle, stream);
        m_cuda_stream = stream;
    }
    const static int im2col_step = 64;
    // const size_t workspaceSize = getWorkspaceSize(inputDesc, 2, outputDesc, 1);

    int batch_size = inputDesc[0].dims.d[0];
    int inputChannel = inputDesc[0].dims.d[1];
    int inputHeight = inputDesc[0].dims.d[2];
    int inputWidth = inputDesc[0].dims.d[3];

    DCN_PARAMS dcn_params;
    dcn_params.cublas_handle = m_cublas_handle;
    dcn_params.batchSize = batch_size;
    dcn_params.inputChannel = inputChannel;
    dcn_params.inputW = inputWidth;
    dcn_params.inputH = inputHeight;
    dcn_params.outputChannel = mOutDim;
    dcn_params.kernelW = mKernelSize.d[0];
    dcn_params.kernelH = mKernelSize.d[1];
    dcn_params.strideW = mStride.d[0];
    dcn_params.strideH = mStride.d[1];
    dcn_params.padW = mPadding.d[0];
    dcn_params.padH = mPadding.d[1];
    dcn_params.dilationW = mDilation.d[0];
    dcn_params.dilationH = mDilation.d[1];
    dcn_params.group = mGroup;
    dcn_params.deformable_group = mDeformableGroup;
    dcn_params.im2col_step = std::min(64, batch_size);

    deform_conv_forward_cuda((float *)inputs[0], (float *)mWdev, (float *)inputs[1],
                             (float *)outputs[0], workSpace,
                             dcn_params,
                             stream);

    return 0;
}

// IPluginV2Ext Methods
nvinfer1::DataType DeformableConvPluginDynamic::getOutputDataType(int index, const nvinfer1::DataType *inputTypes, int nbInputs) const
{
    assert(nbInputs == 2);
    return inputTypes[0];
}

// IPluginV2 Methods
const char *DeformableConvPluginDynamic::getPluginType() const
{
    return DCN_NAME;
}

const char *DeformableConvPluginDynamic::getPluginVersion() const
{
    return DCN_VERSION;
}

int DeformableConvPluginDynamic::getNbOutputs() const
{
    return 1;
}

int DeformableConvPluginDynamic::initialize()
{
    cublasCreate(&m_cublas_handle);
    if (mW.values && mWdev==nullptr)
    {
        // target size
        size_t wordSize = samplesCommon::getElementSize(mType);
        size_t nbBytes = mW.count * wordSize;
        CHECK(cudaMalloc((void **)&mWdev, nbBytes));

        if (mType == DataType::kFLOAT)
        {
            convertAndCopyToDevice(mW, reinterpret_cast<float *>(mWdev));
        }
    }

    return 0;
}

void DeformableConvPluginDynamic::terminate()
{
    gLogVerbose << "DCN Plugin terminate start" << std::endl;

    if(mWdev!=nullptr){
        cudaFree(mWdev);
        mWdev=nullptr;
        cublasDestroy(m_cublas_handle);
    }

    gLogVerbose << "DCN Plugin terminate done" << std::endl;
}

size_t DeformableConvPluginDynamic::getSerializationSize() const
{
    size_t wordSize = samplesCommon::getElementSize(mType);
    return wordSize * mNumParamsW +
           sizeof(mType) +
           sizeof(mOutDim) +
           sizeof(mKernelSize) +
           sizeof(mNumParamsW) +
           sizeof(mStride) +
           sizeof(mPadding) +
           sizeof(mDilation) +
           sizeof(mDeformableGroup) +
           sizeof(mGroup);
}

void DeformableConvPluginDynamic::serialize(void *buffer) const
{
    serialize_value(&buffer, mType);
    serialize_value(&buffer, mOutDim);
    serialize_value(&buffer, mKernelSize);
    serialize_value(&buffer, mNumParamsW);

    serialize_value(&buffer, mStride);
    serialize_value(&buffer, mPadding);
    serialize_value(&buffer, mDilation);
    serialize_value(&buffer, mDeformableGroup);
    serialize_value(&buffer, mGroup);

    size_t wordSize = samplesCommon::getElementSize(mType);
    char *d = static_cast<char *>(buffer);
    serFromHost(d, mW.values, mNumParamsW * wordSize);
}

void DeformableConvPluginDynamic::destroy()
{
    // This gets called when the network containing plugin is destroyed
    delete this;
}

void DeformableConvPluginDynamic::setPluginNamespace(const char *libNamespace)
{
    mNamespace = libNamespace;
}

const char *DeformableConvPluginDynamic::getPluginNamespace() const
{
    return mNamespace.c_str();
}

////////////////////// creator /////////////////////////////

DeformableConvPluginDynamicCreator::DeformableConvPluginDynamicCreator()
{
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

const char *DeformableConvPluginDynamicCreator::getPluginName() const
{
    return DCN_NAME;
}

const char *DeformableConvPluginDynamicCreator::getPluginVersion() const
{
    return DCN_VERSION;
}

const PluginFieldCollection *DeformableConvPluginDynamicCreator::getFieldNames()
{
    return &mFC;
}

IPluginV2 *DeformableConvPluginDynamicCreator::createPlugin(const char *name, const PluginFieldCollection *fc)
{
    int outDims = 0;
    int typeId = -1;
    nvinfer1::Dims kernelSize;

    nvinfer1::Dims stride{2, {1, 1}};
    nvinfer1::Dims padding{2, {0, 0}};
    nvinfer1::Dims dilation{2, {1, 1}};
    int deformableGroup = 1;
    int group = 1;

    nvinfer1::Weights W;
    W.count = 0;
    W.values = nullptr;

    for (int i = 0; i < fc->nbFields; i++)
    {
        if (fc->fields[i].data == nullptr)
        {
            continue;
        }
        std::string field_name(fc->fields[i].name);
        if (field_name.compare("type_id") == 0)
        {
            typeId = static_cast<const int *>(fc->fields[i].data)[0];
        }

        if (field_name.compare("out_dims") == 0)
        {
            outDims = static_cast<const int *>(fc->fields[i].data)[0];
        }

        if (field_name.compare("deformable_group") == 0)
        {
            deformableGroup = static_cast<const int *>(fc->fields[i].data)[0];
        }

        if (field_name.compare("group") == 0)
        {
            group = static_cast<const int *>(fc->fields[i].data)[0];
        }

        if (field_name.compare("kernel_size") == 0)
        {
            kernelSize.nbDims = 2;
            kernelSize.d[0] = static_cast<const int *>(fc->fields[i].data)[0];
            kernelSize.d[1] = static_cast<const int *>(fc->fields[i].data)[1];
        }

        if (field_name.compare("stride") == 0)
        {
            stride.nbDims = 2;
            stride.d[0] = static_cast<const int *>(fc->fields[i].data)[0];
            stride.d[1] = static_cast<const int *>(fc->fields[i].data)[1];
        }

        if (field_name.compare("padding") == 0)
        {
            padding.nbDims = 2;
            padding.d[0] = static_cast<const int *>(fc->fields[i].data)[0];
            padding.d[1] = static_cast<const int *>(fc->fields[i].data)[1];
        }

        if (field_name.compare("dilation") == 0)
        {
            dilation.nbDims = 2;
            dilation.d[0] = static_cast<const int *>(fc->fields[i].data)[0];
            dilation.d[1] = static_cast<const int *>(fc->fields[i].data)[1];
        }

        if (field_name.compare("W") == 0)
        {
            // gLogVerbose << "Building W...\n";
            W.values = fc->fields[i].data;
            W.count = fc->fields[i].length;
            W.type = fieldTypeToDataType(fc->fields[i].type);
        }
    }

    if (outDims <= 0)
    {
        gLogError << "Invalid output dimension" << std::endl;
    }
    if (typeId < 0 || typeId > 3)
    {
        gLogError << "Invalid type id" << typeId << std::endl;
    }
    if (W.count == 0 || W.values == nullptr || W.count < outDims)
    {
        gLogError << "Invalid weights" << std::endl;
    }

    DataType type = static_cast<DataType>(typeId);
    DeformableConvPluginDynamic *plugin = new DeformableConvPluginDynamic(name, type, outDims, kernelSize, W);
    plugin->setPluginNamespace(getPluginNamespace());
    plugin->setStrideNd(stride);
    plugin->setPaddingNd(padding);
    plugin->setDilationNd(dilation);
    plugin->setDeformableGroup(deformableGroup);
    plugin->setGroup(group);

    return plugin;
}

IPluginV2 *DeformableConvPluginDynamicCreator::deserializePlugin(const char *name, const void *serialData, size_t serialLength)
{
    // This object will be deleted when the network is destroyed, which will
    // call FCPluginDynamic::destroy()
    auto plugin = new DeformableConvPluginDynamic(name, serialData, serialLength);
    plugin->setPluginNamespace(getPluginNamespace());
    return plugin;
}

void DeformableConvPluginDynamicCreator::setPluginNamespace(const char *libNamespace)
{
    mNamespace = libNamespace;
}

const char *DeformableConvPluginDynamicCreator::getPluginNamespace() const
{
    return mNamespace.c_str();
}

} // namespace plugin
} // namespace amirstan