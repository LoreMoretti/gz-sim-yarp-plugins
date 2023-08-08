#include <gz/sim/Model.hh>
#include <gz/sim/Util.hh>
#include <gz/sim/System.hh>
#include <gz/plugin/Register.hh>
#include <gz/sim/Sensor.hh>
#include <gz/sim/Link.hh>
#include <yarp/os/LogStream.h>
#include <yarp/os/Network.h>
#include <yarp/dev/PolyDriver.h>
#include <yarp/dev/PolyDriverList.h>
#include <gz/transport/Node.hh>
#include <gz/sim/components/Camera.hh>
#include <gz/sim/components/Sensor.hh>
#include <gz/sim/components/ParentEntity.hh>
#include <gz/sim/components/Name.hh>
#include "CameraDriver.cpp"


using namespace gz;
using namespace sim;
using namespace systems;


class GazeboYarpCamera
      : public System,
        public ISystemConfigure,
        public ISystemPreUpdate,
        public ISystemPostUpdate
        
{
  public:
    
    GazeboYarpCamera() : m_deviceRegistered(false)
    {
    }
    
    virtual ~GazeboYarpCamera()
    {
        if (m_deviceRegistered) 
        {
            Handler::getHandler()->removeDevice(m_deviceScopedName);
            m_deviceRegistered = false;
        }
        
        if(m_cameraDriver.isValid()) 
        {
            m_cameraDriver.close();
        }
        HandlerCamera::getHandler()->removeSensor(sensorScopedName);
        yarp::os::Network::fini();
    }

    virtual void Configure(const Entity &_entity,
                          const std::shared_ptr<const sdf::Element> &_sdf,
                          EntityComponentManager &_ecm,
                          EventManager &/*_eventMgr*/) override
    { 
        yarp::os::Network::init();
        if (!yarp::os::Network::checkNetwork())
        {
            yError() << "Yarp network does not seem to be available, is the yarpserver running?";
            return;
        }

        ::yarp::dev::Drivers::factory().add(new ::yarp::dev::DriverCreatorOf< ::yarp::dev::GazeboYarpCameraDriver>
                                            ("gazebo_camera", "grabber", "GazeboYarpCameraDriver"));
        ::yarp::os::Property driver_properties;

        bool wipe = false;
        if (_sdf->HasElement("yarpConfigurationString"))
        {
            std::string configuration_string = _sdf->Get<std::string>("yarpConfigurationString");
            driver_properties.fromString(configuration_string, wipe);
            if (!driver_properties.check("sensorName"))
            {
                yError() << "GazeboYarpCamera : missing sensorName parameter";
                return;
            }
            if (!driver_properties.check("parentLinkName"))
            {
                yError() << "GazeboYarpCamera : missing parentLinkName parameter";
                return;
            }
            yInfo() << "GazeboYarpCamera: configuration of sensor " << driver_properties.find("sensorName").asString() 
                    << " loaded from yarpConfigurationString : " << configuration_string << "\n";
        }
        else 
        {
            yError() << "GazeboYarpCamera : missing yarpConfigurationString element";
            return; 
        }

        std::string sensorName = driver_properties.find("sensorName").asString();
        std::string parentLinkName = driver_properties.find("parentLinkName").asString();
        
        auto model = Model(_entity);
        auto parentLink = model.LinkByName(_ecm, parentLinkName);
        this->sensor = _ecm.EntityByComponents(
            components::ParentEntity(parentLink),
            components::Name(sensorName),
            components::Sensor());
        auto sdfSensor = _ecm.ComponentData<components::Camera>(sensor).value().Element();
        auto sdfImage = sdfSensor.get()->GetElement("camera").get()->GetElement("image").get();

        cameraData.m_height = sdfImage->Get<int>("height");
        cameraData.m_width = sdfImage->Get<int>("width");
        cameraData.m_bufferSize = 3*cameraData.m_width*cameraData.m_height;

        sensorScopedName = scopedName(this->sensor, _ecm);
        this->cameraData.sensorScopedName = sensorScopedName;

        driver_properties.put(YarpCameraScopedName.c_str(), sensorScopedName.c_str());
        if (!driver_properties.check("yarpDeviceName"))
        {
            yError() << "GazeboYarpCamera : missing yarpDeviceName parameter for device" << sensorScopedName;
            return;
        }

        //Insert the pointer in the singleton handler for retriving it in the yarp driver
        HandlerCamera::getHandler()->setSensor(&(this->cameraData));

        driver_properties.put("device","gazebo_camera");
        driver_properties.put("sensor_name", sensorName);

        //Open the driver
        if(!m_cameraDriver.open(driver_properties)) 
        {
            yError()<<"GazeboYarpCamera Plugin failed: error in opening yarp driver";
            return;
        }
        
        m_cameraDriver.view(iFrameGrabberImage);
        if (iFrameGrabberImage == NULL)
        {
            yError()<< "Unable to get the iFrameGrabberImage interface from the device";
            return; 
        }

        m_deviceScopedName = sensorScopedName + "/" + driver_properties.find("yarpDeviceName").asString();

        if(!Handler::getHandler()->setDevice(m_deviceScopedName, &m_cameraDriver))
        {
            yError()<<"GazeboYarpCamera: failed setting scopedDeviceName(=" << m_deviceScopedName << ")";
            return;
        }
        this->m_deviceRegistered = true;
        this->cameraInitialized = false;
        yInfo() << "GazeboYarpCamera: Registered YARP device with instance name:" << m_deviceScopedName;

    }

    virtual void PreUpdate(const UpdateInfo &_info,
                         EntityComponentManager &_ecm) override
    {
        if(!this->cameraInitialized && _ecm.ComponentData<components::SensorTopic>(sensor).has_value())
        {
            this->cameraInitialized = true;
            auto CameraTopicName = _ecm.ComponentData<components::SensorTopic>(sensor).value();
            this->node.Subscribe(CameraTopicName, &GazeboYarpCamera::CameraCb, this);
        }
    }

    virtual void PostUpdate(const UpdateInfo &_info,
                            const EntityComponentManager &_ecm) override
    {
        gz::msgs::Image cameraMsg;
        {
            std::lock_guard<std::mutex> lock(this->cameraMsgMutex);
            cameraMsg = this->cameraMsg;
        }

        if(this->cameraInitialized)
        {
            std::lock_guard<std::mutex> lock(cameraData.m_mutex);
            memcpy(cameraData.m_imageBuffer, cameraMsg.data().c_str(), cameraData.m_bufferSize);
            cameraData.simTime = _info.simTime.count()/1e9; 
        }
    }

    void CameraCb(const gz::msgs::Image &_msg)
    {
        std::lock_guard<std::mutex> lock(this->cameraMsgMutex);
        cameraMsg = _msg;
    }
    
  private: 
    Entity sensor;
    yarp::dev::PolyDriver m_cameraDriver;
    std::string m_deviceScopedName;
    std::string sensorScopedName;
    bool m_deviceRegistered;
    CameraData cameraData;
    bool cameraInitialized;
    gz::transport::Node node;
    gz::msgs::Image cameraMsg;
    std::mutex cameraMsgMutex;
    yarp::dev::IFrameGrabberImage* iFrameGrabberImage;
};


 
// Register plugin
GZ_ADD_PLUGIN(GazeboYarpCamera,
                    gz::sim::System,
                    GazeboYarpCamera::ISystemConfigure,
                    GazeboYarpCamera::ISystemPreUpdate,
                    GazeboYarpCamera::ISystemPostUpdate)
 
// Add plugin alias so that we can refer to the plugin without the version
// namespace
GZ_ADD_PLUGIN_ALIAS(GazeboYarpCamera, "gz::sim::systems::GazeboYarpCamera")