/* Copyright (c) 2015-2017, EPFL/Blue Brain Project
 * All rights reserved. Do not distribute without permission.
 * Responsible Author: Cyrille Favreau <cyrille.favreau@epfl.ch>
 *
 * This file is part of Brayns <https://github.com/BlueBrain/Brayns>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "ZeroEQPlugin.h"

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>
#include <brayns/Brayns.h>
#include <brayns/common/camera/Camera.h>
#include <brayns/common/engine/Engine.h>
#include <brayns/common/renderer/FrameBuffer.h>
#include <brayns/common/renderer/Renderer.h>
#include <brayns/common/scene/Scene.h>
#include <brayns/common/simulation/AbstractSimulationHandler.h>
#include <brayns/common/volume/VolumeHandler.h>
#include <brayns/io/simulation/CADiffusionSimulationHandler.h>
#include <brayns/io/simulation/SpikeSimulationHandler.h>
#include <brayns/parameters/ParametersManager.h>
#include <brayns/version.h>
#include <fstream>
#include <zerobuf/render/camera.h>
#include <zeroeq/http/helpers.h>

namespace
{
const std::string ENDPOINT_API_VERSION = "v1/";
const std::string ENDPOINT_CAMERA = ENDPOINT_API_VERSION + "camera";
const std::string ENDPOINT_DATA_SOURCE = ENDPOINT_API_VERSION + "data-source";
const std::string ENDPOINT_FORCE_RENDERING =
    ENDPOINT_API_VERSION + "force-rendering";
const std::string ENDPOINT_FRAME_BUFFERS =
    ENDPOINT_API_VERSION + "frame-buffers";
const std::string ENDPOINT_RESET_CAMERA = ENDPOINT_API_VERSION + "reset-camera";
const std::string ENDPOINT_SCENE = ENDPOINT_API_VERSION + "scene";
const std::string ENDPOINT_SETTINGS = ENDPOINT_API_VERSION + "settings";
const std::string ENDPOINT_SIMULATION_HISTOGRAM =
    ENDPOINT_API_VERSION + "simulation-histogram";
const std::string ENDPOINT_SPIKES = ENDPOINT_API_VERSION + "spikes";
const std::string ENDPOINT_VOLUME_HISTOGRAM =
    ENDPOINT_API_VERSION + "volume-histogram";
const std::string ENDPOINT_VERSION = ENDPOINT_API_VERSION + "version";
const std::string ENDPOINT_PROGRESS = ENDPOINT_API_VERSION + "progress";
const std::string ENDPOINT_CLIP_PLANES = ENDPOINT_API_VERSION + "clip-planes";
const std::string ENDPOINT_FRAME = ENDPOINT_API_VERSION + "frame";
const std::string ENDPOINT_IMAGE_JPEG = ENDPOINT_API_VERSION + "image-jpeg";
const std::string ENDPOINT_MATERIAL_LUT = ENDPOINT_API_VERSION + "material-lut";
const std::string ENDPOINT_VIEWPORT = ENDPOINT_API_VERSION + "viewport";
const std::string ENDPOINT_CIRCUIT_CONFIG_BUILDER =
    ENDPOINT_API_VERSION + "circuit-config-builder";

const size_t NB_MAX_MESSAGES = 20; // Maximum number of ZeroEQ messages to read
                                   // between each rendering loop
}

namespace brayns
{
ZeroEQPlugin::ZeroEQPlugin(ParametersManager& parametersManager)
    : ExtensionPlugin()
    , _parametersManager(parametersManager)
    , _compressor(tjInitCompress())
    , _processingImageJpeg(false)
    , _remoteProgress(0)
    , _dirtyEngine(false)
{
    _setupHTTPServer();
    _setupRequests();
    _setupSubscriber();
    _initializeDataSource();
    _initializeSettings();
}

ZeroEQPlugin::~ZeroEQPlugin()
{
    if (_compressor)
        tjDestroy(_compressor);
}

std::string ZeroEQPlugin::getEndpointAPIVersion() const
{
    return ENDPOINT_API_VERSION;
}

void ZeroEQPlugin::_onNewEngine()
{
    if (_httpServer)
    {
        servus::Serializable& cam = *_engine->getCamera().getSerializable();
        _httpServer->handle(ENDPOINT_CAMERA, cam);
    }

    _requests[ ::brayns::v1::Camera::ZEROBUF_TYPE_IDENTIFIER()] = [&] {
        return _publisher.publish(*_engine->getCamera().getSerializable());
    };

    _engine->extensionInit(*this);
    _dirtyEngine = false;
}

void ZeroEQPlugin::_onChangeEngine()
{
    servus::Serializable& cam = *_engine->getCamera().getSerializable();
    cam.registerDeserializedCallback(
        servus::Serializable::DeserializedCallback());

    if (_httpServer)
        _httpServer->remove(ENDPOINT_CAMERA);

    _requests.erase(::brayns::v1::Camera::ZEROBUF_TYPE_IDENTIFIER());

    try
    {
        _engine->recreate();
    }
    catch (const std::runtime_error&)
    {
    }

    _forceRendering = true;
}

bool ZeroEQPlugin::run(EngineWeakPtr engine_, KeyboardHandler&,
                       AbstractManipulator&)
{
    if (engine_.expired())
        return true;

    if (_engine != engine_.lock().get() || _dirtyEngine)
    {
        _engine = engine_.lock().get();
        _onNewEngine();
    }

    const auto& ap = _parametersManager.getApplicationParameters();
    if (ap.getAutoPublishZeroEQEvents())
    {
        if (_requestVolumeHistogram())
            _publisher.publish(_remoteVolumeHistogram);

        if (_requestSimulationHistogram())
            _publisher.publish(_remoteSimulationHistogram);

        if (_requestFrame())
            _publisher.publish(_remoteFrame);
    }

    // In the case of interactions with Jupyter notebooks, HTTP messages are
    // received in a blocking and sequential manner, meaning that the subscriber
    // never has more than one message in its queue. In other words, only one
    // message is processed between each rendering loop. The following code
    // allows the processing of several messages and performs rendering after
    // NB_MAX_MESSAGES reads, or if one of the messages forces rendering by
    // setting the _forceRedering boolean variable to true.
    _forceRendering = false;
    for (size_t i = 0; i < NB_MAX_MESSAGES && !_forceRendering; ++i)
        while (_subscriber.receive(0))
        {
        }
    return !_dirtyEngine;
}

bool ZeroEQPlugin::operator!() const
{
    return !_httpServer;
}

::zeroeq::http::Server* ZeroEQPlugin::operator->()
{
    return _httpServer.get();
}

void ZeroEQPlugin::handle(servus::Serializable& object)
{
    if (_httpServer)
        _httpServer->handle(object);
    _subscriber.subscribe(object);
    _requests[object.getTypeIdentifier()] = [&] {
        return _publisher.publish(object);
    };

    // publish updates from HTTP to subscribers, e.g. livreGUI
    object.registerDeserializedCallback([&] { _publisher.publish(object); });
}

bool ZeroEQPlugin::_writeBlueConfigFile(const std::string& blueConfigFilename,
                                        const strings& params)
{
    std::ofstream blueConfig(blueConfigFilename);
    if (!blueConfig.good())
        return false;

    std::map<std::string, std::string> dictionary = {
        {"morphology_folder", "MorphologyPath"}, {"mvd_file", "CircuitPath"}};

    blueConfig << "Run Default" << std::endl << "{" << std::endl;
    auto parameters = params;
    for (auto& parameter : parameters)
    {
        strings keyValue;
        boost::char_separator<char> separator("=");
        boost::tokenizer<boost::char_separator<char>> tokens(parameter,
                                                             separator);
        for_each(tokens.begin(), tokens.end(),
                 [&keyValue](const std::string& s) { keyValue.push_back(s); });

        if (keyValue.size() != 2)
        {
            BRAYNS_ERROR << "BlueConfigBuilder: Invalid parameter " << parameter
                         << std::endl;
            continue;
        }
        if (dictionary.find(keyValue[0]) == dictionary.end())
        {
            BRAYNS_ERROR << "BlueConfigBuilder: Unknown parameter "
                         << keyValue[0] << std::endl;
            continue;
        }
        blueConfig << dictionary[keyValue[0]] << " " << keyValue[1]
                   << std::endl;
    }
    blueConfig << "}" << std::endl;
    blueConfig.close();
    return true;
}

std::future<::zeroeq::http::Response> ZeroEQPlugin::_handleCircuitConfigBuilder(
    const ::zeroeq::http::Request& request)
{
    const auto paramsAsString = request.query;
    strings params;
    boost::split(params, paramsAsString, boost::is_any_of("&"));
    const std::string blueConfigFilename =
        _parametersManager.getApplicationParameters().getTmpFolder() +
        "/BlueConfig";
    if (_writeBlueConfigFile(blueConfigFilename, params))
    {
        const std::string body =
            "{\"filename\":\"" + blueConfigFilename + "\"}";
        return ::zeroeq::http::make_ready_response(::zeroeq::http::Code::OK,
                                                   body, "application/json");
    }
    else
        return ::zeroeq::http::make_ready_response(
            ::zeroeq::http::Code::SERVICE_UNAVAILABLE);
}

void ZeroEQPlugin::_setupHTTPServer()
{
    const strings& arguments =
        _parametersManager.getApplicationParameters().arguments();
    char** argv = new char*[arguments.size()];
    for (size_t i = 0; i < arguments.size(); ++i)
        argv[i] = const_cast<char*>(arguments[i].c_str());

    _httpServer = ::zeroeq::http::Server::parse(arguments.size(),
                                                const_cast<const char**>(argv),
                                                _subscriber);
    delete[] argv;

    if (!_httpServer)
    {
        BRAYNS_ERROR << "HTTP could not be initialized" << std::endl;
        return;
    }

    BRAYNS_INFO << "Registering handlers on " << _httpServer->getURI()
                << std::endl;

    _httpServer->handleGET(ENDPOINT_VERSION, brayns::Version::getSchema(),
                           &brayns::Version::toJSON);

    _httpServer->handleGET(ENDPOINT_IMAGE_JPEG, _remoteImageJPEG);
    _remoteImageJPEG.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestImageJPEG, this));

    _httpServer->handleGET(ENDPOINT_FRAME_BUFFERS, _remoteFrameBuffers);
    _remoteFrameBuffers.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestFrameBuffers, this));

    _httpServer->handlePUT(ENDPOINT_RESET_CAMERA, _remoteResetCamera);
    _remoteResetCamera.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_resetCameraUpdated, this));

    _httpServer->handle(ENDPOINT_SCENE, _remoteScene);
    _remoteScene.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_sceneUpdated, this));
    _remoteScene.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestScene, this));

    _httpServer->handle(ENDPOINT_SPIKES, _remoteSpikes);
    _remoteSpikes.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_spikesUpdated, this));
    _remoteSpikes.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestSpikes, this));

    _httpServer->handle(ENDPOINT_MATERIAL_LUT, _remoteMaterialLUT);
    _remoteMaterialLUT.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_materialLUTUpdated, this));
    _remoteMaterialLUT.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestMaterialLUT, this));

    _httpServer->handle(ENDPOINT_DATA_SOURCE, _remoteDataSource);
    _remoteDataSource.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_dataSourceUpdated, this));

    _httpServer->handle(ENDPOINT_SETTINGS, _remoteSettings);
    _remoteSettings.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_settingsUpdated, this));

    _httpServer->handle(ENDPOINT_FRAME, _remoteFrame);
    _remoteFrame.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestFrame, this));
    _remoteFrame.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_frameUpdated, this));

    _httpServer->handle(ENDPOINT_VIEWPORT, _remoteViewport);
    _remoteViewport.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestViewport, this));
    _remoteViewport.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_viewportUpdated, this));

    _httpServer->handle(::zeroeq::http::Method::GET,
                        ENDPOINT_CIRCUIT_CONFIG_BUILDER,
                        std::bind(&ZeroEQPlugin::_handleCircuitConfigBuilder,
                                  this, std::placeholders::_1));

    _httpServer->handle(ENDPOINT_CLIP_PLANES, _clipPlanes);
    _clipPlanes.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_clipPlanesUpdated, this));
    _clipPlanes.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestClipPlanes, this));

    _httpServer->handleGET(ENDPOINT_SIMULATION_HISTOGRAM,
                           _remoteSimulationHistogram);
    _remoteSimulationHistogram.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestSimulationHistogram, this));

    _httpServer->handleGET(ENDPOINT_VOLUME_HISTOGRAM, _remoteVolumeHistogram);
    _remoteVolumeHistogram.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestVolumeHistogram, this));

    _httpServer->handle(ENDPOINT_FORCE_RENDERING, _remoteForceRendering);
    _remoteForceRendering.registerDeserializedCallback(
        std::bind(&ZeroEQPlugin::_forceRenderingUpdated, this));

    _httpServer->handleGET(ENDPOINT_PROGRESS, _remoteProgress);
    _remoteProgress.registerSerializeCallback(
        std::bind(&ZeroEQPlugin::_requestProgress, this));
}

void ZeroEQPlugin::_setupRequests()
{
    _requests[ ::lexis::render::ImageJPEG::ZEROBUF_TYPE_IDENTIFIER()] = [&] {
        _requestImageJPEG();
        return _publisher.publish(_remoteImageJPEG);
    };

    _requests[ ::lexis::render::Frame::ZEROBUF_TYPE_IDENTIFIER()] = [&] {
        _requestFrame();
        return _publisher.publish(_remoteFrame);
    };

    _requests[v1::FrameBuffers::ZEROBUF_TYPE_IDENTIFIER()] = [&] {
        _requestFrameBuffers();
        return _publisher.publish(_remoteFrameBuffers);
    };

    ::lexis::render::ClipPlanes clipPlanes;
    _requests[clipPlanes.getTypeIdentifier()] =
        std::bind(&ZeroEQPlugin::_requestClipPlanes, this);

    _requests[v1::Spikes::ZEROBUF_TYPE_IDENTIFIER()] = [&] {
        _requestSpikes();
        return _publisher.publish(_remoteSpikes);
    };
}

void ZeroEQPlugin::_setupSubscriber()
{
    _subscriber.subscribe(_remoteMaterialLUT);
    _subscriber.subscribe(_clipPlanes);
    _subscriber.subscribe(_remoteFrame);
}

void ZeroEQPlugin::_resetCameraUpdated()
{
    auto& sceneParameters = _parametersManager.getSceneParameters();
    _engine->getCamera().setEnvironmentMap(
        !sceneParameters.getEnvironmentMap().empty());
    _engine->getCamera().reset();
}

bool ZeroEQPlugin::_requestScene()
{
    auto& ms = _remoteScene.getMaterials();
    ms.clear();
    auto& scene = _engine->getScene();
    auto& materials = scene.getMaterials();

    for (size_t materialId = NB_SYSTEM_MATERIALS; materialId < materials.size();
         ++materialId)
    {
        // Materials
        auto& material = scene.getMaterial(materialId);
        ::brayns::v1::Material m;
        m.setDiffuseColor(material.getColor());
        m.setSpecularColor(material.getSpecularColor());
        m.setSpecularExponent(material.getSpecularExponent());
        m.setReflectionIndex(material.getReflectionIndex());
        m.setOpacity(material.getOpacity());
        m.setRefractionIndex(material.getRefractionIndex());
        m.setLightEmission(material.getEmission());
        m.setGlossiness(material.getGlossiness());
        ms.push_back(m);
    }

    const auto& boundsMin = scene.getWorldBounds().getMin();
    const auto& boundsMax = scene.getWorldBounds().getMax();
    _remoteScene.setBounds({{boundsMin.begin(), boundsMin.end()},
                            {boundsMax.begin(), boundsMax.end()}});
    return true;
}

void ZeroEQPlugin::_sceneUpdated()
{
    if (!_engine->isReady())
        return;

    auto& materials = _remoteScene.getMaterials();
    auto& scene = _engine->getScene();

    for (size_t materialId = 0; materialId < materials.size(); ++materialId)
    {
        // Materials
        auto& material = scene.getMaterial(materialId + NB_SYSTEM_MATERIALS);
        ::brayns::v1::Material& m = materials[materialId];
        const auto& diffuse = m.getDiffuseColor();
        Vector3f kd = {diffuse[0], diffuse[1], diffuse[2]};
        material.setColor(kd);

        const auto& specular = m.getSpecularColor();
        Vector3f ks = {specular[0], specular[1], specular[2]};
        material.setSpecularColor(ks);

        material.setSpecularExponent(m.getSpecularExponent());
        material.setReflectionIndex(m.getReflectionIndex());
        material.setOpacity(m.getOpacity());
        material.setRefractionIndex(m.getRefractionIndex());
        material.setEmission(m.getLightEmission());
        material.setGlossiness(m.getGlossiness());
    }

    scene.commitMaterials(true);
}

void ZeroEQPlugin::_spikesUpdated()
{
#if 0
    AbstractSimulationHandlerPtr simulationHandler =
        _engine->getScene().getSimulationHandler();

    SpikeSimulationHandler* spikeSimulationHandler =
        dynamic_cast< SpikeSimulationHandler * >(simulationHandler.get());

    if( spikeSimulationHandler )
    {
        uint64_t ts = _remoteSpikes.getTimestamp();
        float* data = (float*)spikeSimulationHandler->getFrameData( ts );
        for( const auto& gid: _remoteSpikes.getGidsVector() )
            data[gid] = ts;

        _engine->getFrameBuffer().clear();
        _engine->getScene().commitSimulationData();
    }
#endif
}

void ZeroEQPlugin::_materialLUTUpdated()
{
    if (!_engine->isReady())
        return;

    auto& scene = _engine->getScene();
    TransferFunction& transferFunction = scene.getTransferFunction();

    transferFunction.clear();
    auto& diffuseColors = transferFunction.getDiffuseColors();

    const auto size = _remoteMaterialLUT.getDiffuse().size();
    if (_remoteMaterialLUT.getAlpha().size() != size)
    {
        BRAYNS_ERROR << "All lookup tables must have the same size. "
                     << "Event will be ignored" << std::endl;
        return;
    }

    for (size_t i = 0; i < size; ++i)
    {
        const auto& diffuse = _remoteMaterialLUT.getDiffuse()[i];
        const auto alpha = _remoteMaterialLUT.getAlpha()[i];
        Vector4f color = {diffuse.getRed(), diffuse.getGreen(),
                          diffuse.getBlue(), alpha};
        diffuseColors.push_back(color);
    }

    auto& emissionIntensities = transferFunction.getEmissionIntensities();
    for (const auto& emission : _remoteMaterialLUT.getEmission())
    {
        const Vector3f intensity = {emission.getRed(), emission.getGreen(),
                                    emission.getBlue()};
        emissionIntensities.push_back(intensity);
    }

    auto& contributions = transferFunction.getContributions();
    for (const auto& contribution : _remoteMaterialLUT.getContribution())
        contributions.push_back(contribution);

    const auto& range = _remoteMaterialLUT.getRange();
    if (range[0] == range[1])
        transferFunction.setValuesRange(Vector2f(0.f, 255.f));
    else
        transferFunction.setValuesRange(Vector2f(range[0], range[1]));
    scene.commitTransferFunctionData();
}

void ZeroEQPlugin::_requestMaterialLUT()
{
    auto& scene = _engine->getScene();
    TransferFunction& transferFunction = scene.getTransferFunction();

    _remoteMaterialLUT.getEmission().resize(
        transferFunction.getEmissionIntensities().size());
    _remoteMaterialLUT.getDiffuse().resize(
        transferFunction.getDiffuseColors().size());
    _remoteMaterialLUT.getAlpha().resize(
        transferFunction.getDiffuseColors().size());
    _remoteMaterialLUT.getContribution().resize(
        transferFunction.getContributions().size());

    size_t j = 0;
    for (const auto& diffuseColor : transferFunction.getDiffuseColors())
    {
        ::lexis::render::Color color(diffuseColor.x(), diffuseColor.y(),
                                     diffuseColor.z());
        _remoteMaterialLUT.getDiffuse()[j] = color;
        _remoteMaterialLUT.getAlpha()[j] = diffuseColor.w();
        ++j;
    }

    j = 0;
    if (transferFunction.getEmissionIntensities().empty())
    {
        _remoteMaterialLUT.getEmission().resize(
            transferFunction.getDiffuseColors().size());
        for (size_t i = 0; i < transferFunction.getDiffuseColors().size(); ++i)
            _remoteMaterialLUT.getEmission()[j++] = {0, 0, 0};
    }
    else
        for (const auto& emission : transferFunction.getEmissionIntensities())
        {
            ::lexis::render::Color color(emission.x(), emission.y(),
                                         emission.z());
            _remoteMaterialLUT.getEmission()[j] = color;
            ++j;
        }

    j = 0;
    if (transferFunction.getContributions().empty())
    {
        _remoteMaterialLUT.getContribution().resize(
            transferFunction.getDiffuseColors().size());
        for (size_t i = 0; i < transferFunction.getDiffuseColors().size(); ++i)
            _remoteMaterialLUT.getContribution()[j++] = 0.f;
    }
    else
        for (const auto& contribution : transferFunction.getContributions())
            _remoteMaterialLUT.getContribution()[j++] = contribution;

    const auto& range = transferFunction.getValuesRange();
    std::vector<double> rangeVector = {range.x(), range.y()};
    _remoteMaterialLUT.setRange(rangeVector);
}

void ZeroEQPlugin::_resizeImage(unsigned int* srcData, const Vector2i& srcSize,
                                const Vector2i& dstSize, uints& dstData)
{
    dstData.reserve(dstSize.x() * dstSize.y());
    size_t x_ratio =
        static_cast<size_t>(((srcSize.x() << 16) / dstSize.x()) + 1);
    size_t y_ratio =
        static_cast<size_t>(((srcSize.y() << 16) / dstSize.y()) + 1);

    for (int y = 0; y < dstSize.y(); ++y)
    {
        for (int x = 0; x < dstSize.x(); ++x)
        {
            const size_t x2 = ((x * x_ratio) >> 16);
            const size_t y2 = ((y * y_ratio) >> 16);
            dstData[(y * dstSize.x()) + x] = srcData[(y2 * srcSize.x()) + x2];
        }
    }
}

bool ZeroEQPlugin::_requestImageJPEG()
{
    if (!_processingImageJpeg)
    {
        _processingImageJpeg = true;
        const auto& newFrameSize =
            _parametersManager.getApplicationParameters().getJpegSize();
        if (newFrameSize.x() == 0 || newFrameSize.y() == 0)
        {
            BRAYNS_ERROR << "Encountered invalid size of image JPEG: "
                         << newFrameSize << std::endl;
            _processingImageJpeg = false;
            return false;
        }

        FrameBuffer& frameBuffer = _engine->getFrameBuffer();
        const auto& frameSize = frameBuffer.getSize();
        unsigned int* colorBuffer = (unsigned int*)frameBuffer.getColorBuffer();
        if (colorBuffer)
        {
            unsigned int* resizedColorBuffer = colorBuffer;

            uints resizedBuffer;
            if (frameSize != newFrameSize)
            {
                _resizeImage(colorBuffer, frameSize, newFrameSize,
                             resizedBuffer);
                resizedColorBuffer = resizedBuffer.data();
            }

            int32_t pixelFormat = TJPF_RGBX;
            switch (frameBuffer.getFrameBufferFormat())
            {
            case FrameBufferFormat::bgra_i8:
                pixelFormat = TJPF_BGRX;
                break;
            case FrameBufferFormat::rgba_i8:
            default:
                pixelFormat = TJPF_RGBX;
            }

            unsigned long jpegSize = 0;
            uint8_t* jpegData = _encodeJpeg((uint32_t)newFrameSize.x(),
                                            (uint32_t)newFrameSize.y(),
                                            (uint8_t*)resizedColorBuffer,
                                            pixelFormat, jpegSize);

            _remoteImageJPEG.setData(jpegData, jpegSize);
            tjFree(jpegData);
        }
        _processingImageJpeg = false;
    }
    return true;
}

bool ZeroEQPlugin::_requestFrameBuffers()
{
    auto& frameBuffer = _engine->getFrameBuffer();
    const Vector2i frameSize = frameBuffer.getSize();
    const float* depthBuffer = frameBuffer.getDepthBuffer();

    _remoteFrameBuffers.setWidth(frameSize.x());
    _remoteFrameBuffers.setHeight(frameSize.y());
    if (depthBuffer)
    {
        uint16_ts depths;
        const size_t size = frameSize.x() * frameSize.y();
        depths.reserve(size);
        for (size_t i = 0; i < size; ++i)
            depths.push_back(std::numeric_limits<uint16_t>::max() *
                             depthBuffer[i]);
        _remoteFrameBuffers.setDepth(reinterpret_cast<const uint8_t*>(
                                         depths.data()),
                                     depths.size() * sizeof(uint16_t));
    }
    else
        _remoteFrameBuffers.setDepth(0, 0);

    const uint8_t* colorBuffer = frameBuffer.getColorBuffer();
    if (colorBuffer)
        _remoteFrameBuffers.setDiffuse(colorBuffer,
                                       frameSize.x() * frameSize.y() *
                                           frameBuffer.getColorDepth());
    else
        _remoteFrameBuffers.setDiffuse(0, 0);

    return true;
}

bool ZeroEQPlugin::_requestSpikes()
{
#if 0
    AbstractSimulationHandlerPtr simulationHandler = _engine->getScene().getSimulationHandler();

    SpikeSimulationHandler* spikeSimulationHandler =
        dynamic_cast< SpikeSimulationHandler * >( simulationHandler.get() );

    std::vector< uint64_t > spikeGids;
    uint64_t ts = 0.f;

    _remoteSpikes.setTimestamp( ts );
    if( spikeSimulationHandler )
    {
        uint64_t frameSize = spikeSimulationHandler->getFrameSize();
        uint64_t* gids = (uint64_t*)spikeSimulationHandler->getFrameData( ts );
        spikeGids.reserve( frameSize );
        spikeGids.assign( gids, gids + frameSize );
    }
    _remoteSpikes.setGids( spikeGids );
#endif
    return true;
}

void ZeroEQPlugin::_initializeDataSource()
{
    auto& sceneParameters = _parametersManager.getSceneParameters();
    auto& geometryParameters = _parametersManager.getGeometryParameters();
    auto& volumeParameters = _parametersManager.getVolumeParameters();

    _remoteDataSource.setColorMapFile(sceneParameters.getColorMapFilename());
    _remoteDataSource.setMorphologyFolder(
        geometryParameters.getMorphologyFolder());
    _remoteDataSource.setNestCircuit(geometryParameters.getNESTCircuit());
    _remoteDataSource.setNestReport(geometryParameters.getNESTReport());
    _remoteDataSource.setPdbFile(geometryParameters.getPDBFile());
    _remoteDataSource.setPdbFolder(geometryParameters.getPDBFolder());
    _remoteDataSource.setXyzbFile(geometryParameters.getXYZBFile());
    _remoteDataSource.setMeshFolder(geometryParameters.getMeshFolder());
    _remoteDataSource.setMeshFile(geometryParameters.getMeshFile());

    // Circuit
    ::brayns::v1::CircuitConfiguration circuit;
    circuit.setCircuitConfigFile(geometryParameters.getCircuitConfiguration());
    circuit.setDensity(geometryParameters.getCircuitDensity());
    const Boxf& aabb = geometryParameters.getCircuitBoundingBox();
    if (aabb.getSize() != 0)
    {
        const floats values = {aabb.getMin().x(), aabb.getMin().y(),
                               aabb.getMin().z(), aabb.getMax().x(),
                               aabb.getMin().y(), aabb.getMin().z()};
        circuit.setBoundingBox(values);
    }
    circuit.setMeshFilenamePattern(
        geometryParameters.getCircuitMeshFilenamePattern());
    circuit.setMeshFolder(geometryParameters.getCircuitMeshFolder());
    circuit.setUseSimulationModel(
        geometryParameters.getCircuitUseSimulationModel());
    circuit.setTargets(geometryParameters.getCircuitTargets());
    circuit.setReport(geometryParameters.getCircuitReport());
    circuit.setStartSimulationTime(
        geometryParameters.getCircuitStartSimulationTime());
    circuit.setEndSimulationTime(
        geometryParameters.getCircuitEndSimulationTime());
    circuit.setSimulationStep(geometryParameters.getCircuitSimulationStep());
    circuit.setSimulationValuesRange(
        geometryParameters.getCircuitSimulationValuesRange());
    circuit.setMeshTransformation(
        geometryParameters.getCircuitMeshTransformation());
    circuit.setHistogramSize(
        geometryParameters.getCircuitSimulationHistogramSize());
    _remoteDataSource.setCircuitConfiguration(circuit);

    // Connectivity
    ::brayns::v1::ConnectivityConfiguration connectivityConfiguration;
    connectivityConfiguration.setFilename(
        geometryParameters.getConnectivityFile());
    connectivityConfiguration.setMatrixId(
        geometryParameters.getConnectivityMatrixId());
    connectivityConfiguration.setShowConnections(
        geometryParameters.getConnectivityShowConnections());
    const auto& connectivityDimensionRange =
        geometryParameters.getConnectivityDimensionRange();
    const uint32_ts dimensions = {connectivityDimensionRange.x(),
                                  connectivityDimensionRange.y()};
    connectivityConfiguration.setDimensionRange(dimensions);
    const auto& connectivityScale = geometryParameters.getConnectivityScale();
    const floats range = {connectivityScale.x(), connectivityScale.y(),
                          connectivityScale.z()};
    connectivityConfiguration.setScale(range);
    _remoteDataSource.setConnectivityConfiguration(connectivityConfiguration);

    // Other parameters
    _remoteDataSource.setLoadCacheFile(geometryParameters.getLoadCacheFile());
    _remoteDataSource.setSaveCacheFile(geometryParameters.getSaveCacheFile());
    _remoteDataSource.setRadiusMultiplier(
        geometryParameters.getRadiusMultiplier());
    _remoteDataSource.setRadiusCorrection(
        geometryParameters.getRadiusCorrection());
    _remoteDataSource.setColorScheme(
        ::brayns::v1::ColorScheme(geometryParameters.getColorScheme()));
    _remoteDataSource.setSceneEnvironment(::brayns::v1::SceneEnvironment(
        geometryParameters.getSceneEnvironment()));
    _remoteDataSource.setGeometryQuality(
        ::brayns::v1::GeometryQuality(geometryParameters.getGeometryQuality()));
    _remoteDataSource.setNestCacheFile(geometryParameters.getNESTCacheFile());

    const auto mst = geometryParameters.getMorphologySectionTypes();
    std::vector<::brayns::v1::SectionType> sectionTypes;
    const auto all = mst & (size_t)::brayns::v1::SectionType::all;
    if (all || mst & (size_t)::brayns::v1::SectionType::soma)
        sectionTypes.push_back(::brayns::v1::SectionType::soma);
    if (all || mst & (size_t)::brayns::v1::SectionType::axon)
        sectionTypes.push_back(::brayns::v1::SectionType::axon);
    if (all || mst & (size_t)::brayns::v1::SectionType::dendrite)
        sectionTypes.push_back(::brayns::v1::SectionType::dendrite);
    if (all || mst & (size_t)::brayns::v1::SectionType::apical_dendrite)
        sectionTypes.push_back(::brayns::v1::SectionType::apical_dendrite);
    _remoteDataSource.setMorphologySectionTypes(sectionTypes);

    const auto& morphologyLayout = geometryParameters.getMorphologyLayout();
    brayns::v1::MorphologyLayout remoteMorphologyLayout(
        morphologyLayout.nbColumns, morphologyLayout.verticalSpacing,
        morphologyLayout.horizontalSpacing);
    _remoteDataSource.setMorphologyLayout(remoteMorphologyLayout);

    _remoteDataSource.setGenerateMultipleModels(
        geometryParameters.getGenerateMultipleModels());
    _remoteDataSource.setVolumeFolder(volumeParameters.getFolder());
    _remoteDataSource.setVolumeFile(volumeParameters.getFilename());
    _remoteDataSource.setVolumeDimensions(
        Vector3ui(volumeParameters.getDimensions()));
    _remoteDataSource.setVolumeElementSpacing(
        Vector3f(volumeParameters.getElementSpacing()));
    _remoteDataSource.setVolumeOffset(Vector3f(volumeParameters.getOffset()));
    _remoteDataSource.setEnvironmentMap(sceneParameters.getEnvironmentMap());
    _remoteDataSource.setMolecularSystemConfig(
        geometryParameters.getMolecularSystemConfig());
    _remoteDataSource.setMetaballsGridSize(
        geometryParameters.getMetaballsGridSize());
    _remoteDataSource.setMetaballsThreshold(
        geometryParameters.getMetaballsThreshold());
    _remoteDataSource.setMetaballsSamplesFromSoma(
        geometryParameters.getMetaballsSamplesFromSoma());
    _remoteDataSource.setMemoryMode(geometryParameters.getMemoryMode() ==
                                            MemoryMode::shared
                                        ? brayns::v1::MemoryMode::shared
                                        : brayns::v1::MemoryMode::replicated);

    _remoteDataSource.setSceneFile(geometryParameters.getSceneFile());
}

void ZeroEQPlugin::_dataSourceUpdated()
{
    if (!_engine->isReady())
        return;

    auto& geometryParameters = _parametersManager.getGeometryParameters();

    _parametersManager.set("splash-scene-folder",
                           ""); // Make sure the splash scene is removed
    _parametersManager.set("color-map-file",
                           _remoteDataSource.getColorMapFileString());
    _parametersManager.set("morphology-folder",
                           _remoteDataSource.getMorphologyFolderString());
    _parametersManager.set("nest-circuit",
                           _remoteDataSource.getNestCircuitString());
    _parametersManager.set("nest-report",
                           _remoteDataSource.getNestReportString());
    _parametersManager.set("pdb-file", _remoteDataSource.getPdbFileString());
    _parametersManager.set("pdb-folder",
                           _remoteDataSource.getPdbFolderString());
    _parametersManager.set("xyzb-file", _remoteDataSource.getXyzbFileString());
    _parametersManager.set("mesh-folder",
                           _remoteDataSource.getMeshFolderString());
    _parametersManager.set("mesh-file", _remoteDataSource.getMeshFileString());
    _parametersManager.set("circuit-config",
                           _remoteDataSource.getCircuitConfiguration()
                               .getCircuitConfigFileString());
    _parametersManager.set("load-cache-file",
                           _remoteDataSource.getLoadCacheFileString());
    _parametersManager.set("save-cache-file",
                           _remoteDataSource.getSaveCacheFileString());
    _parametersManager.set("radius-multiplier",
                           std::to_string(
                               _remoteDataSource.getRadiusMultiplier()));
    _parametersManager.set("radius-correction",
                           std::to_string(
                               _remoteDataSource.getRadiusCorrection()));
    _parametersManager.set("color-scheme",
                           geometryParameters.getColorSchemeAsString(
                               static_cast<ColorScheme>(
                                   _remoteDataSource.getColorScheme())));
    _parametersManager.set("scene-environment",
                           geometryParameters.getSceneEnvironmentAsString(
                               static_cast<SceneEnvironment>(
                                   _remoteDataSource.getSceneEnvironment())));
    _parametersManager.set("geometry-quality",
                           geometryParameters.getGeometryQualityAsString(
                               static_cast<GeometryQuality>(
                                   _remoteDataSource.getGeometryQuality())));
    _parametersManager.set(
        "circuit-targets",
        _remoteDataSource.getCircuitConfiguration().getTargetsString());
    _parametersManager.set(
        "circuit-report",
        _remoteDataSource.getCircuitConfiguration().getReportString());
    _parametersManager.set(
        "circuit-density",
        std::to_string(
            _remoteDataSource.getCircuitConfiguration().getDensity()));
    floats aabb(
        _remoteDataSource.getCircuitConfiguration().getBoundingBoxVector());
    if (aabb.size() == 6)
        _parametersManager.set(
            "circuit-bounding-box",
            std::to_string(aabb[0]) + " " + std::to_string(aabb[1]) + " " +
                std::to_string(aabb[2]) + " " + std::to_string(aabb[3]) + " " +
                std::to_string(aabb[4]) + " " + std::to_string(aabb[5]));
    else
        _parametersManager.set("circuit-bounding-box", "0 0 0 0 0 0");

    _parametersManager.set(
        "circuit-mesh-folder",
        _remoteDataSource.getCircuitConfiguration().getMeshFolderString());
    _parametersManager.set(
        "circuit-mesh-transformation",
        (_remoteDataSource.getCircuitConfiguration().getMeshTransformation())
            ? "1"
            : "0");
    _parametersManager.set("circuit-start-simulation-time",
                           std::to_string(
                               _remoteDataSource.getCircuitConfiguration()
                                   .getStartSimulationTime()));
    _parametersManager.set("circuit-end-simulation-time",
                           std::to_string(
                               _remoteDataSource.getCircuitConfiguration()
                                   .getEndSimulationTime()));
    _parametersManager.set(
        "circuit-simulation-step",
        std::to_string(
            _remoteDataSource.getCircuitConfiguration().getSimulationStep()));
    _parametersManager.set(
        "circuit-simulation-values-range",
        std::to_string(_remoteDataSource.getCircuitConfiguration()
                           .getSimulationValuesRange()[0]) +
            " " + std::to_string(_remoteDataSource.getCircuitConfiguration()
                                     .getSimulationValuesRange()[1]));
    _parametersManager.set(
        "circuit-simulation-histogram-size",
        std::to_string(
            _remoteDataSource.getCircuitConfiguration().getHistogramSize()));
    _parametersManager.set("nest-cache-file",
                           _remoteDataSource.getNestCacheFileString());
    _parametersManager.set(
        "circuit-uses-simulation-model",
        _remoteDataSource.getCircuitConfiguration().getUseSimulationModel()
            ? "1"
            : "0");

    uint morphologySectionTypes = size_t(MorphologySectionType::undefined);
    const auto& sectionTypes = _remoteDataSource.getMorphologySectionTypes();
    for (const auto& sectionType : sectionTypes)
    {
        switch (sectionType)
        {
        case ::brayns::v1::SectionType::soma:
            morphologySectionTypes |= size_t(MorphologySectionType::soma);
            break;
        case ::brayns::v1::SectionType::axon:
            morphologySectionTypes |= size_t(MorphologySectionType::axon);
            break;
        case ::brayns::v1::SectionType::dendrite:
            morphologySectionTypes |= size_t(MorphologySectionType::dendrite);
            break;
        case ::brayns::v1::SectionType::apical_dendrite:
            morphologySectionTypes |=
                size_t(MorphologySectionType::apical_dendrite);
            break;
        case ::brayns::v1::SectionType::all:
            morphologySectionTypes |= size_t(MorphologySectionType::all);
        }
    }
    _parametersManager.set("morphology-section-types",
                           std::to_string(morphologySectionTypes));

    const auto remoteMorphologyLayout = _remoteDataSource.getMorphologyLayout();
    std::string layoutAsString;
    layoutAsString += std::to_string(remoteMorphologyLayout.getNbColumns());
    layoutAsString +=
        " " + std::to_string(remoteMorphologyLayout.getVerticalSpacing());
    layoutAsString +=
        " " + std::to_string(remoteMorphologyLayout.getHorizontalSpacing());
    _parametersManager.set("morphology-layout", layoutAsString);

    _parametersManager.set("generate-multiple-models",
                           (_remoteDataSource.getGenerateMultipleModels()
                                ? "1"
                                : "0"));
    _parametersManager.set("volume-folder",
                           _remoteDataSource.getVolumeFolderString());
    _parametersManager.set("volume-file",
                           _remoteDataSource.getVolumeFileString());
    _parametersManager.set(
        "volume-dimensions",
        std::to_string(_remoteDataSource.getVolumeDimensions()[0]) + " " +
            std::to_string(_remoteDataSource.getVolumeDimensions()[1]) + " " +
            std::to_string(_remoteDataSource.getVolumeDimensions()[2]));
    _parametersManager.set(
        "volume-element-spacing",
        std::to_string(_remoteDataSource.getVolumeElementSpacing()[0]) + " " +
            std::to_string(_remoteDataSource.getVolumeElementSpacing()[1]) +
            " " +
            std::to_string(_remoteDataSource.getVolumeElementSpacing()[2]));
    _parametersManager.set(
        "volume-offset",
        std::to_string(_remoteDataSource.getVolumeOffset()[0]) + " " +
            std::to_string(_remoteDataSource.getVolumeOffset()[1]) + " " +
            std::to_string(_remoteDataSource.getVolumeOffset()[2]));
    _parametersManager.set("environment-map",
                           _remoteDataSource.getEnvironmentMapString());

    _parametersManager.set("molecular-system-config",
                           _remoteDataSource.getMolecularSystemConfigString());

    _parametersManager.set("metaballs-grid-size",
                           std::to_string(
                               _remoteDataSource.getMetaballsGridSize()));
    _parametersManager.set("metaballs-threshold",
                           std::to_string(
                               _remoteDataSource.getMetaballsThreshold()));
    _parametersManager.set(
        "metaballs-samples-from-soma",
        std::to_string(_remoteDataSource.getMetaballsSamplesFromSoma()));

    _parametersManager.set("memory-mode", _remoteDataSource.getMemoryMode() ==
                                                  brayns::v1::MemoryMode::shared
                                              ? "shared"
                                              : "replicated");
    _parametersManager.set("scene-file",
                           _remoteDataSource.getSceneFileString());
    _parametersManager.set("circuit-mesh-filename-pattern",
                           _remoteDataSource.getCircuitConfiguration()
                               .getMeshFilenamePatternString());

    _parametersManager.set(
        "connectivity-file",
        _remoteDataSource.getConnectivityConfiguration().getFilenameString());
    _parametersManager.set(
        "connectivity-matrix-id",
        std::to_string(
            _remoteDataSource.getConnectivityConfiguration().getMatrixId()));
    _parametersManager.set(
        "connectivity-show-connections",
        (_remoteDataSource.getConnectivityConfiguration().getShowConnections()
             ? "1"
             : "0"));
    const auto& connectivityDimensionRange =
        _remoteDataSource.getConnectivityConfiguration().getDimensionRange();
    _parametersManager.set("connectivity-dimension-range",
                           std::to_string(connectivityDimensionRange[0]) + " " +
                               std::to_string(connectivityDimensionRange[1]));
    const auto& connectivityScale =
        _remoteDataSource.getConnectivityConfiguration().getScale();
    _parametersManager.set("connectivity-scale",
                           std::to_string(connectivityScale[0]) + " " +
                               std::to_string(connectivityScale[1]) + " " +
                               std::to_string(connectivityScale[2]));
    _parametersManager.print();

    _engine->buildScene();
}

void ZeroEQPlugin::_initializeSettings()
{
    auto& renderingParameters = _parametersManager.getRenderingParameters();
    auto& volumeParameters = _parametersManager.getVolumeParameters();
    auto& applicationParameters = _parametersManager.getApplicationParameters();

    if (renderingParameters.getEngine() == "ospray")
        _remoteSettings.setEngine(::brayns::v1::Engine::ospray);
    else if (renderingParameters.getEngine() == "optix")
        _remoteSettings.setEngine(::brayns::v1::Engine::optix);
    else if (renderingParameters.getEngine() == "livre")
        _remoteSettings.setEngine(::brayns::v1::Engine::livre);
    _remoteSettings.setVolumeSamplesPerRay(volumeParameters.getSamplesPerRay());

    switch (renderingParameters.getRenderer())
    {
    case RendererType::proximity:
        _remoteSettings.setShader(::brayns::v1::Shader::proximity);
        break;
    case RendererType::particle:
        _remoteSettings.setShader(::brayns::v1::Shader::particle);
        break;
    case RendererType::simulation:
        _remoteSettings.setShader(::brayns::v1::Shader::simulation);
        break;
    case RendererType::geometryNormals:
        _remoteSettings.setShader(::brayns::v1::Shader::geometry_normals);
        break;
    case RendererType::shadingNormals:
        _remoteSettings.setShader(::brayns::v1::Shader::shading_normals);
        break;
    default:
        _remoteSettings.setShader(::brayns::v1::Shader::basic);
        break;
    }

    switch (renderingParameters.getShading())
    {
    case ShadingType::diffuse:
        _remoteSettings.setShading(::brayns::v1::Shading::diffuse);
        break;
    case ShadingType::electron:
        _remoteSettings.setShading(::brayns::v1::Shading::electron);
        break;
    default:
        _remoteSettings.setShading(::brayns::v1::Shading::none);
        break;
    }
    _remoteSettings.setSamplesPerPixel(
        renderingParameters.getSamplesPerPixel());
    _remoteSettings.setAmbientOcclusion(
        renderingParameters.getAmbientOcclusionStrength());
    _remoteSettings.setAccumulation(renderingParameters.getAccumulation());
    _remoteSettings.setShadows(renderingParameters.getShadows());
    _remoteSettings.setSoftShadows(renderingParameters.getSoftShadows());
    _remoteSettings.setRadiance(
        renderingParameters.getLightEmittingMaterials());
    Vector3f value = renderingParameters.getBackgroundColor();
    _remoteSettings.setBackgroundColor(value);
    _remoteSettings.setDetectionDistance(
        renderingParameters.getDetectionDistance());
    value = renderingParameters.getDetectionNearColor();
    _remoteSettings.setDetectionNearColor(value);
    value = renderingParameters.getDetectionFarColor();
    _remoteSettings.setDetectionFarColor(value);
    _remoteSettings.setEpsilon(renderingParameters.getEpsilon());
    _remoteSettings.setHeadLight(renderingParameters.getHeadLight());
    _remoteSettings.setJpegCompression(
        applicationParameters.getJpegCompression());
    const auto& jpegSize = applicationParameters.getJpegSize();
    _remoteSettings.setJpegSize({jpegSize[0], jpegSize[1]});
    _remoteSettings.setFrameExportFolder(
        applicationParameters.getFrameExportFolder());
    _remoteSettings.setSynchronousMode(
        applicationParameters.getSynchronousMode());
    _remoteSettings.setVarianceThreshold(
        renderingParameters.getVarianceThreshold());
}

void ZeroEQPlugin::_settingsUpdated()
{
    const auto& renderingParameters =
        _parametersManager.getRenderingParameters();

    switch (_remoteSettings.getEngine())
    {
    case ::brayns::v1::Engine::ospray:
        _parametersManager.getRenderingParameters().setEngine("ospray");
        break;
    case ::brayns::v1::Engine::optix:
        _parametersManager.getRenderingParameters().setEngine("optix");
        break;
    case ::brayns::v1::Engine::livre:
        _parametersManager.getRenderingParameters().setEngine("livre");
        break;
    }

    _parametersManager.set("volume-samples-per-ray",
                           std::to_string(
                               _remoteSettings.getVolumeSamplesPerRay()));
    _parametersManager.set("renderer", renderingParameters.getRendererAsString(
                                           static_cast<RendererType>(
                                               _remoteSettings.getShader())));

    switch (_remoteSettings.getShading())
    {
    case ::brayns::v1::Shading::diffuse:
        _parametersManager.set("shading", "diffuse");
        break;
    case ::brayns::v1::Shading::electron:
        _parametersManager.set("shading", "electron");
        break;
    default:
        _parametersManager.set("shading", "none");
        break;
    }
    _parametersManager.set("samples-per-pixel",
                           std::to_string(
                               _remoteSettings.getSamplesPerPixel()));
    _parametersManager.set("ambient-occlusion",
                           std::to_string(
                               _remoteSettings.getAmbientOcclusion()));
    _parametersManager.set("accumulation",
                           (_remoteSettings.getAccumulation() ? "1" : "0"));
    _parametersManager.set("shadows",
                           std::to_string(_remoteSettings.getShadows()));
    _parametersManager.set("soft-shadows",
                           std::to_string(_remoteSettings.getSoftShadows()));
    _parametersManager.set("radiance",
                           (_remoteSettings.getRadiance() ? "1" : "0"));
    _parametersManager.set(
        "background-color",
        std::to_string(_remoteSettings.getBackgroundColor()[0]) + " " +
            std::to_string(_remoteSettings.getBackgroundColor()[1]) + " " +
            std::to_string(_remoteSettings.getBackgroundColor()[2]));
    _parametersManager.set("detection-distance",
                           std::to_string(
                               _remoteSettings.getDetectionDistance()));
    _parametersManager.set("detection-on-different-material",
                           (_remoteSettings.getDetectionOnDifferentMaterial()
                                ? "1"
                                : "0"));
    _parametersManager.set(
        "detection-near-color",
        std::to_string(_remoteSettings.getDetectionNearColor()[0]) + " " +
            std::to_string(_remoteSettings.getDetectionNearColor()[1]) + " " +
            std::to_string(_remoteSettings.getDetectionNearColor()[2]));
    _parametersManager.set(
        "detection-far-color",
        std::to_string(_remoteSettings.getDetectionFarColor()[0]) + " " +
            std::to_string(_remoteSettings.getDetectionFarColor()[1]) + " " +
            std::to_string(_remoteSettings.getDetectionFarColor()[2]));
    _parametersManager.set("epsilon",
                           std::to_string(_remoteSettings.getEpsilon()));
    _parametersManager.set("head-light",
                           (_remoteSettings.getHeadLight() ? "1" : "0"));

    auto& app = _parametersManager.getApplicationParameters();
    app.setJpegSize(Vector2ui{_remoteSettings.getJpegSize()});
    app.setJpegCompression(
        std::min(_remoteSettings.getJpegCompression(), 100u));

    if (_engine->name() !=
        _parametersManager.getRenderingParameters().getEngine())
    {
        _onChangeEngine();
    }

    _parametersManager.set("synchronous-mode",
                           (_remoteSettings.getSynchronousMode() ? "1" : "0"));
    _parametersManager.set("variance-threshold",
                           std::to_string(
                               _remoteSettings.getVarianceThreshold()));
    _parametersManager.set("frame-export-folder",
                           _remoteSettings.getFrameExportFolderString());
    if (_remoteSettings.getFrameExportFolderString().empty())
        _engine->resetFrameNumber();
}

bool ZeroEQPlugin::_requestFrame()
{
    auto caSimHandler = _engine->getScene().getCADiffusionSimulationHandler();
    auto simHandler = _engine->getScene().getSimulationHandler();
    auto volHandler = _engine->getScene().getVolumeHandler();
    uint64_t nbFrames = simHandler
                            ? simHandler->getNbFrames()
                            : (volHandler ? volHandler->getNbFrames() : 0);
    nbFrames =
        std::max(nbFrames, caSimHandler ? caSimHandler->getNbFrames() : 0);

    const auto& sceneParams = _parametersManager.getSceneParameters();
    const auto current =
        nbFrames == 0 ? 0 : (sceneParams.getAnimationFrame() % nbFrames);
    const auto animationDelta = sceneParams.getAnimationDelta();

    if (current == _remoteFrame.getCurrent() &&
        nbFrames == _remoteFrame.getEnd() &&
        animationDelta == _remoteFrame.getDelta())
    {
        return false;
    }

    _remoteFrame.setCurrent(current);
    _remoteFrame.setDelta(animationDelta);
    _remoteFrame.setEnd(nbFrames);
    _remoteFrame.setStart(0);
    return true;
}

void ZeroEQPlugin::_frameUpdated()
{
    auto& sceneParams = _parametersManager.getSceneParameters();
    sceneParams.setAnimationFrame(_remoteFrame.getCurrent());
    sceneParams.setAnimationDelta(_remoteFrame.getDelta());

    CADiffusionSimulationHandlerPtr handler =
        _engine->getScene().getCADiffusionSimulationHandler();
    if (handler && _engine->isReady())
    {
        auto& scene = _engine->getScene();
        handler->setFrame(scene, _remoteFrame.getCurrent());
        scene.setSpheresDirty(true);
        scene.serializeGeometry();
        scene.commit();
    }
}

bool ZeroEQPlugin::_requestViewport()
{
    const auto& windowSize =
        _parametersManager.getApplicationParameters().getWindowSize();
    _remoteViewport.setSize({windowSize[0], windowSize[1]});
    return true;
}

void ZeroEQPlugin::_viewportUpdated()
{
    _parametersManager.getApplicationParameters().setWindowSize(
        Vector2ui{_remoteViewport.getSize()});
}

bool ZeroEQPlugin::_requestSimulationHistogram()
{
    auto simulationHandler = _engine->getScene().getSimulationHandler();
    if (!simulationHandler || !simulationHandler->histogramChanged())
        return false;

    const auto& histogram = simulationHandler->getHistogram();
    _remoteSimulationHistogram.setMin(histogram.range.x());
    _remoteSimulationHistogram.setMax(histogram.range.y());
    _remoteSimulationHistogram.setBins(histogram.values);
    return true;
}

bool ZeroEQPlugin::_requestVolumeHistogram()
{
    auto volumeHandler = _engine->getScene().getVolumeHandler();
    if (!volumeHandler)
        return false;

    const auto& histogram = volumeHandler->getHistogram();
    _remoteVolumeHistogram.setMin(histogram.range.x());
    _remoteVolumeHistogram.setMax(histogram.range.y());
    _remoteVolumeHistogram.setBins(histogram.values);
    return true;
}

void ZeroEQPlugin::_clipPlanesUpdated()
{
    const auto& bounds = _engine->getScene().getWorldBounds();
    const auto& size = bounds.getSize();
    ClipPlanes clipPlanes;

    for (const auto& plane : _clipPlanes.getPlanes())
    {
        const auto& normal = plane.getNormal();
        const auto distance = plane.getD() * size.find_max();
        clipPlanes.push_back(
            Vector4f(normal[0], normal[1], normal[2], distance));
    }
    if (clipPlanes.size() == 6)
    {
        _engine->getCamera().setClipPlanes(clipPlanes);
        _publisher.publish(_clipPlanes);
    }
    else
        BRAYNS_ERROR << "Invalid number of clip planes. Expected 6, received "
                     << clipPlanes.size() << std::endl;
}

bool ZeroEQPlugin::_requestClipPlanes()
{
    std::vector<::lexis::render::detail::Plane> planes;
    const auto& clipPlanes = _engine->getCamera().getClipPlanes();
    for (const auto& clipPlane : clipPlanes)
    {
        ::lexis::render::detail::Plane plane;
        float normal[3] = {clipPlane.x(), clipPlane.y(), clipPlane.z()};
        plane.setNormal(normal);
        plane.setD(clipPlane.w());
        planes.push_back(plane);
    }
    _clipPlanes.setPlanes(planes);
    return true;
}

void ZeroEQPlugin::_forceRenderingUpdated()
{
    _forceRendering = true;
}

bool ZeroEQPlugin::_requestProgress()
{
    _remoteProgress.setAmount(_engine->getLastProgress());
    _remoteProgress.setOperation(_engine->getLastOperation());
    return true;
}

uint8_t* ZeroEQPlugin::_encodeJpeg(const uint32_t width, const uint32_t height,
                                   const uint8_t* rawData,
                                   const int32_t pixelFormat,
                                   unsigned long& dataSize)
{
    uint8_t* tjSrcBuffer = const_cast<uint8_t*>(rawData);
    const int32_t color_components = 4; // Color Depth
    const int32_t tjPitch = width * color_components;
    const int32_t tjPixelFormat = pixelFormat;

    uint8_t* tjJpegBuf = 0;
    const int32_t tjJpegSubsamp = TJSAMP_444;
    const int32_t tjFlags = TJXOP_ROT180;

    const int32_t success = tjCompress2(
        _compressor, tjSrcBuffer, width, tjPitch, height, tjPixelFormat,
        &tjJpegBuf, &dataSize, tjJpegSubsamp,
        _parametersManager.getApplicationParameters().getJpegCompression(),
        tjFlags);

    if (success != 0)
    {
        BRAYNS_ERROR << "libjpeg-turbo image conversion failure" << std::endl;
        return 0;
    }
    return static_cast<uint8_t*>(tjJpegBuf);
}
}
