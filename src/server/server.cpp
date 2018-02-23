#include <Poco/ConsoleChannel.h>
#include <Poco/SimpleFileChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/Logger.h>
#include <Poco/AutoPtr.h>
#include <Poco/Message.h>
#include <Poco/Exception.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionCallback.h>
#include <Poco/Util/HelpFormatter.h>

#include "ogrsf_frmts.h"

#include <stdexcept>
#include <iostream>

#include "server/server.h"
#include "server/http/listener.h"
#include "common/macros.h"
#include "common/config.h"
#include "server/broker.h"


using namespace Batyr;


Server::Server()
    :   Poco::Util::ServerApplication(),
        _helpRequested(false),
        _configOk(false)
{
}


int
Server::main(const std::vector<std::string> & args)
{
    UNUSED(args)

    // exit early if the user just wanted to see 
    // the help text
    if (_helpRequested) {
        return Poco::Util::Application::EXIT_OK;
    }

    // check the config file
    if (!_configOk) {
        return Poco::Util::Application::EXIT_USAGE;
    }

    if (!initLogging()) {
        return Poco::Util::Application::EXIT_USAGE;
    }
    Poco::Logger & logger = Poco::Logger::get("Server"); 

    // initialize ogr
#if GDAL_VERSION_MAJOR > 1
    GDALAllRegister();
#else
    OGRRegisterAll();
#endif

    // make GML attributes available. Otherwise some attibutes listed in the GML
    // might not exist in the read features.
    // This option is available with gdal 1.11, but it won't cause an issue if it
    // is also set for earlier versions
    // http://trac.osgeo.org/gdal/ticket/5418
    CPLSetConfigOption("GML_ATTRIBUTES_TO_OGR_FIELDS", "YES");

    try {
        Broker broker(configuration);

        auto httplistener_ptr = std::make_shared<Batyr::Http::Listener>(configuration);
        broker.addListener(httplistener_ptr);
        broker.run();

        waitForTerminationRequest();  // wait for CTRL-C or kill

        broker.stop();
    }
    catch (Poco::Exception& e) {
        poco_error(logger, e.displayText());
        std::cerr << e.displayText() << std::endl;

        return Poco::Util::Application::EXIT_SOFTWARE;
    }
    catch (std::exception& e) {
        poco_error(logger, e.what());
        std::cerr << e.what() << std::endl;

        return Poco::Util::Application::EXIT_SOFTWARE;
    }
    return Poco::Util::Application::EXIT_OK;
};


bool
Server::initLogging()
{
    // initialize the logging system
    Poco::AutoPtr<Poco::Channel> pChan;
    
    std::string logfile = configuration->getLogFile();
    if (logfile.empty()) {
        pChan.assign(new Poco::ConsoleChannel);
    }
    else {
        // MEMO: there is also the more sophisticated FileChannel logger
        try {
            pChan.assign(new Poco::SimpleFileChannel(logfile));

            // try to set the flush property. not all versions of poco support this
            try {
                pChan->setProperty("flush", "true");
            }
            catch (...) {}

            // immediately open the logfile to check here if the file can be written to
            pChan->open();
        }
        catch (Poco::Exception &e) {
            std::cerr << "Could not setup logfile: " << e.displayText() << std::endl;
            return false;
        }
    }
    Poco::AutoPtr<Poco::PatternFormatter> pPF(new Poco::PatternFormatter);

    // formatting patterns 
    // see http://www.appinf.com/docs/poco/Poco.PatternFormatter.html
    pPF->setProperty("pattern", "%Y-%m-%d %H:%M:%S [%p] [%s] %t");
    Poco::AutoPtr<Poco::FormattingChannel> pFC(new Poco::FormattingChannel(pPF, pChan));
    Poco::Logger::root().setChannel(pFC);

    Poco::Logger::root().setLevel(configuration->getLogLevel());

    return true;
}


void
Server::defineOptions(Poco::Util::OptionSet & options)
{
    Poco::Util::ServerApplication::defineOptions(options);

    options.addOption( Poco::Util::Option("help", "h", "Display help information and exit.")
            .required(false)
            .repeatable(false)
            .callback( Poco::Util::OptionCallback<Server>( this, &Server::handleHelp )));

    options.addOption( Poco::Util::Option("configfile", "c", "Path to the configuration file.")
            .required(true)
            .repeatable(false)
            .argument("file")
            .callback( Poco::Util::OptionCallback<Server>( this, &Server::handleConfigfile )));
}


void
Server::displayHelp()
{
    Poco::Util::HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("-c=CONFIGFILE [OPTIONS]");
    helpFormatter.setHeader(
        "On-demand synchronization of vector datasources to a PostgreSQL/PostGIS database."
        "\n"
        "\n"
        "version: " VERSION_FULL " [git: " VERSION_GIT_FULL "]"
    );
    helpFormatter.format(std::cout);

}


void
Server::handleHelp(const std::string& name, const std::string& value)
{
    UNUSED(name)
    UNUSED(value)

    _helpRequested = true;
    displayHelp();
    stopOptionsProcessing();
}


void
Server::handleConfigfile(const std::string& name, const std::string& value)
{
    UNUSED(name)

    try {
        configuration = std::make_shared<Configuration>(value);
        _configOk = true;
    }
    catch (Batyr::ConfigurationError &e) {
        std::cerr << "Configuration error: " << e.what() << std::endl;
        _configOk = false;
        stopOptionsProcessing();
    }
}
