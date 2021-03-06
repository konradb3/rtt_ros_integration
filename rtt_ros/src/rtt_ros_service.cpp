#include <cstdlib>
#include <list>
#include <queue>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/version.hpp>

#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/tree.h>

#include <rtt/RTT.hpp>
#include <rtt/plugin/ServicePlugin.hpp>
#include <rtt/internal/GlobalService.hpp>

#include <rtt/deployment/ComponentLoader.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/Logger.hpp>
#include <rtt/os/StartStopManager.hpp>
#include <rtt/plugin/PluginLoader.hpp>
#include <rtt/types/TypekitRepository.hpp>

#include <ros/ros.h>
#include <rospack/rospack.h>

#include <rtt_ros/time.h>

using namespace RTT;
using namespace std;

/**
 * The globally loadable ROS service.
 */
class ROSService : public RTT::Service {
public:
  int protocol_id;
  /**
   * Instantiates this service.
   * @param owner The owner or null in case of global.
   */
  ROSService(TaskContext* owner) 
    : Service("ros", owner)
  {
    this->doc("RTT service for loading RTT plugins ");

    // ROS Package-importing
    this->addOperation("import", &ROSService::import, this).doc(
        "Imports the Orocos plugins from a given ROS package (if found) along with the plugins of all of the package's run or exec dependencies as listed in the package.xml.").arg(
            "package", "The ROS package name.");

    this->provides("time")->addOperation("now", &rtt_ros::time::now).doc(
        "Get a ros::Time structure based on the RTT time source.");
  }

  /**
   * Returns a ConnPolicy object for streaming to or from 
   * the given ROS topic. No buffering is done.
   */
  bool import(const std::string& package) 
  {
    RTT::Logger::In in("ROSService::import(\""+package+"\")");

    boost::shared_ptr<RTT::ComponentLoader> loader = RTT::ComponentLoader::Instance();

    bool found_packages = false;

    // Get the dependencies for a given ROS package --> deps
    try {
      // Get the package paths
      std::vector<std::string> ros_package_paths;
      rospack::Rospack rpack;
      rpack.setQuiet(true);
      rpack.getSearchPathFromEnv(ros_package_paths);

      RTT::log(RTT::Debug) << "Loading dependencies for ROS package \""<< package << "\" from: ";
      for(size_t i=0; i<ros_package_paths.size(); i++) { RTT::log(RTT::Debug) << ros_package_paths[i]; }
      RTT::log(RTT::Debug) << RTT::endlog();

      rpack.crawl(ros_package_paths,true);

      if ( ros_package_paths.size() == 0 ) {
        RTT::log(RTT::Error) << "No paths in the ROS_PACKAGE_PATH environment variable! Could not load ROS package \"" << package << "\"" << RTT::endlog();
        return false;
      }

      // Add all rtt_ros/plugin_depend dependencies to package names
      std::vector<std::string> deps_to_import;
      rpack.setQuiet(true);

      // Read the package.xml for this package
      std::queue<std::string> dep_names;
      dep_names.push(package);
      deps_to_import.push_back(package);

      const xmlChar * rtt_plugin_depend_xpath = xmlCharStrdup("/package/export/rtt_ros/plugin_depend/text()");

      while(!dep_names.empty()) 
      {
        namespace fs = boost::filesystem;

        // Get the next dep name, store the dep path
        std::string dep_name, dep_path;
        dep_name = dep_names.front();
        dep_names.pop();

        bool dep_found = rpack.find(dep_name,dep_path);

        if(!dep_found) {
          RTT::log(RTT::Error) << "Could not find ROS package \""<< dep_name << "\" in ROS_PACKAGE_PATH environment variable." <<RTT::endlog();
          continue;
        }

        // Construct the package.xml path
        fs::path package_xml_path = fs::path(dep_path) / "package.xml";

        // Read in package.xml
        if(boost::filesystem::is_regular_file( package_xml_path.string() ) ) {
          xmlInitParser();

          // libxml structures
          xmlDocPtr package_doc;
          xmlXPathContextPtr xpath_ctx; 
          xmlXPathObjectPtr xpath_obj; 

          // Load package.xml
          package_doc = xmlParseFile(package_xml_path.string().c_str());
          xpath_ctx = xmlXPathNewContext(package_doc);

          // Get the text of the rtt_ros <plugin_depend>s
          xpath_obj = xmlXPathEvalExpression(rtt_plugin_depend_xpath, xpath_ctx);

          // Iterate through the nodes
          if(xmlXPathNodeSetIsEmpty(xpath_obj->nodesetval)) {
            RTT::log(RTT::Debug) << "ROS package \""<< dep_name << "\" has no RTT plugin dependencies." <<RTT::endlog();
          } else {
            RTT::log(RTT::Debug) << "ROS package \""<< dep_name << "\" has "<<xpath_obj->nodesetval->nodeNr<<" RTT plugin dependencies." <<RTT::endlog();

            for(int i=0; i < xpath_obj->nodesetval->nodeNr; i++) {
              if(xpath_obj->nodesetval->nodeTab[i]) {
                std::ostringstream oss;
                oss << xmlNodeGetContent(xpath_obj->nodesetval->nodeTab[i]);
                RTT::log(RTT::Debug) << "Found dependency \""<< oss.str() << "\"" <<RTT::endlog();
                dep_names.push(oss.str());

                // Add the dep to the list of deps to import
                deps_to_import.push_back(oss.str());
              }
            }
          }

          xmlXPathFreeObject(xpath_obj);
          xmlXPathFreeContext(xpath_ctx); 
          xmlFreeDoc(package_doc); 
          xmlCleanupParser();
        } else {
          RTT::log(RTT::Error) << "package.xml file for ROS package \""<< dep_name << "\" not found at "<<package_xml_path <<RTT::endlog();
        }
      }

      RTT::log(RTT::Debug) << "Attempting to load RTT plugins from "<<deps_to_import.size()<<" packages..." << RTT::endlog();

      // Import each package dependency
      for(std::vector<std::string>::reverse_iterator it = deps_to_import.rbegin(); 
          it != deps_to_import.rend(); 
          ++it) 
      {
        // Check if it's already been imported
        if(*it == "rtt_ros" || loader->isImported(*it)) {
          RTT::log(RTT::Debug) << "Package dependency '"<< *it <<"' already imported." << RTT::endlog();
          continue;
        }

        // Import the dependency
        if(loader->import(*it,"")) {
          found_packages = true;
        } else {
          RTT::log(RTT::Debug) << "Could not load ROS package dependency \"" << *it << "\" of ROS package \"" << package << "\"" << RTT::endlog();
        }
      }

      // Now that all deps are done, import the package itself:
      if(package == "rtt_ros" || loader->isImported(package)) {
          RTT::log(RTT::Debug) << "Package '"<< package <<"' already imported." << RTT::endlog();
      } else {
        RTT::log(RTT::Debug) << "Importing Orocos components from ROS package '"<<package<<"'" << RTT::endlog();
        if(loader->import(package,"")) {
          found_packages = true;
        } else {
          RTT::log(RTT::Debug) << "Could not import any plugins from ROS package \"" << package << "\"" << RTT::endlog();
        }
      }
    } catch(std::string arg) {
      RTT::log(RTT::Debug) << "While processing the dependencies of " << package << ": not a ros package: " << arg << RTT::endlog();
    }

    if(!found_packages) { 
      RTT::log(RTT::Error) << "Could not load and plugins from ROS package \"" << package << "\" or it's dependencies." << RTT::endlog();
    }

    return found_packages;
  }

};

void loadROSService(){
  RTT::Service::shared_ptr rts(new ROSService(0));
  RTT::internal::GlobalService::Instance()->addService(rts);
}

using namespace RTT;
extern "C" {
  bool loadRTTPlugin(RTT::TaskContext* c){
    loadROSService();
    return true;
  }
  std::string getRTTPluginName (){
    return "ros";
  }
  std::string getRTTTargetName (){
    return OROCOS_TARGET_NAME;
  }
}
