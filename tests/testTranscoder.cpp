#include "../src/modules/liveMediaInput/SourceManager.hh"
#include "../src/modules/liveMediaOutput/SinkManager.hh"
#include "../src/modules/videoResampler/VideoResampler.hh"
#include "../src/modules/videoEncoder/VideoEncoderX264.hh"
#include "../src/AudioFrame.hh"
#include "../src/Controller.hh"
#include "../src/Utils.hh"

#include <csignal>
#include <vector>
#include <string>

#define V_MEDIUM "video"
#define PROTOCOL "RTP"
#define V_PAYLOAD 96
#define V_CODEC "H264"
#define V_BANDWITH 1200
#define V_TIME_STMP_FREQ 90000
#define FRAME_RATE 25

#define A_MEDIUM "audio"
#define A_PAYLOAD 97
#define A_CODEC "OPUS"
#define A_BANDWITH 128
#define A_TIME_STMP_FREQ 48000
#define A_CHANNELS 2

#define OUT_A_CODEC MP3

#define RETRIES 60

bool run = true;

void signalHandler( int signum )
{
    utils::infoMsg("Interruption signal received");

    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    pipe->stop();
    run = false;

    utils::infoMsg("Workers Stopped");
}

void addAudioPath(unsigned port)
{    
    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    
    int aDecId = rand();
    int aEncId = rand();
    int decId = rand();
    int encId = rand();
    std::vector<int> ids({decId, encId});

    AudioDecoderLibav *decoder;
    AudioEncoderLibav *encoder;

    Worker* aDec;
    Worker* aEnc;
    
    Path *path;
    
    //NOTE: Adding decoder to pipeManager and handle worker
    decoder = new AudioDecoderLibav();
    pipe->addFilter(decId, decoder);
    aDec = new Worker();
    aDec->addProcessor(decId, decoder);
    decoder->setWorkerId(aDecId);
    pipe->addWorker(aDecId, aDec);

    //NOTE: Adding encoder to pipeManager and handle worker
    encoder = new AudioEncoderLibav();
    if (!encoder->setup(OUT_A_CODEC, A_CHANNELS, A_TIME_STMP_FREQ)) {
        utils::errorMsg("Error configuring audio encoder. Check provided parameters");
        return;
    }
    pipe->addFilter(encId, encoder);
    aEnc = new Worker();
    aEnc->addProcessor(encId, encoder);
    encoder->setWorkerId(aEncId);
    pipe->addWorker(aEncId, aEnc);
    
    //NOTE: add filter to path
    path = pipe->createPath(pipe->getReceiverID(), pipe->getTransmitterID(), port, -1, ids);
    pipe->addPath(port, path);
    pipe->connectPath(path);

    pipe->startWorkers();
    
    utils::infoMsg("Audio path created from port " + std::to_string(port));
}

void addVideoPath(unsigned port, unsigned width = 0, unsigned height = 0)
{    
    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    
    int wResId = rand();
    int wEncId = rand();
    int wDecId = rand();
    int decId = rand();
    int resId = rand();
    int encId = rand();
    std::vector<int> ids({decId, resId, encId});

    VideoResampler *resampler;
    VideoEncoderX264 *encoder;
    VideoDecoderLibav *decoder;

    Worker* wDec;
    Worker* wRes;
    Worker* wEnc;
    
    Path *path;

    //NOTE: Adding decoder to pipeManager and handle worker
    decoder = new VideoDecoderLibav();
    pipe->addFilter(decId, decoder);
    wDec = new Worker();
    wDec->addProcessor(decId, decoder);
    decoder->setWorkerId(wDecId);
    pipe->addWorker(wDecId, wDec);

    //NOTE: Adding resampler to pipeManager and handle worker
    resampler = new VideoResampler();
    pipe->addFilter(resId, resampler);
    wRes = new Worker();
    wRes->addProcessor(resId, resampler);
    resampler->setWorkerId(wResId);
    resampler->configure(width, height, 0, YUV420P);
    pipe->addWorker(wResId, wRes);

    //NOTE: Adding encoder to pipeManager and handle worker
    encoder = new VideoEncoderX264();
    pipe->addFilter(encId, encoder);
    wEnc = new Worker();
    wEnc->addProcessor(encId, encoder);
    encoder->setWorkerId(wEncId);
    pipe->addWorker(wEncId, wEnc);

    //NOTE: add filter to path
    path = pipe->createPath(pipe->getReceiverID(), pipe->getTransmitterID(), port, -1, ids);
    pipe->addPath(port, path);
    pipe->connectPath(path);

    pipe->startWorkers();
    
    utils::infoMsg("Video path created from port " + std::to_string(port));
}

bool addVideoSDPSession(unsigned port, std::string codec = V_CODEC)
{
    Session *session;
    std::string sessionId;
    std::string sdp;

    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    SourceManager *receiver = pipe->getReceiver();

    sessionId = utils::randomIdGenerator(ID_LENGTH);
    sdp = SourceManager::makeSessionSDP(sessionId, "this is a video stream");
    sdp += SourceManager::makeSubsessionSDP(V_MEDIUM, PROTOCOL, V_PAYLOAD, codec,
                                            V_BANDWITH, V_TIME_STMP_FREQ, port);
    utils::infoMsg(sdp);

    session = Session::createNew(*(receiver->envir()), sdp, sessionId);
    if (!receiver->addSession(session)){
        utils::errorMsg("Could not add video session");
        return false;
    }
    if (!session->initiateSession(receiver)){
        utils::errorMsg("Could not initiate video session");
        return false;
    }
    
    return true;
}

bool addAudioSDPSession(unsigned port, std::string codec = A_CODEC,
                        unsigned channels = A_CHANNELS, unsigned freq = A_TIME_STMP_FREQ)
{
    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    SourceManager *receiver = pipe->getReceiver();
    
    Session *session;
    std::string sessionId;
    std::string sdp;

    sessionId = utils::randomIdGenerator(ID_LENGTH);
    sdp = SourceManager::makeSessionSDP(sessionId, "this is an audio stream");
    sdp += SourceManager::makeSubsessionSDP(A_MEDIUM, PROTOCOL, A_PAYLOAD, codec,
                                            A_BANDWITH, freq, port, channels);
    utils::infoMsg(sdp);

    session = Session::createNew(*(receiver->envir()), sdp, sessionId);
    if (!receiver->addSession(session)){
        utils::errorMsg("Could not add audio session");
        return false;
    }
    if (!session->initiateSession(receiver)){
        utils::errorMsg("Could not initiate audio session");
        return false;
    }
    
    return true;
}

bool addRTSPsession(std::string rtspUri)
{
    Session* session;
    std::string sessionId = utils::randomIdGenerator(ID_LENGTH);
    std::string medium;
    unsigned retries = 0;
    
    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    SourceManager *receiver = pipe->getReceiver();
    
    session = Session::createNewByURL(*(receiver->envir()), "testTranscoder", rtspUri, sessionId);
    if (!receiver->addSession(session)){
        utils::errorMsg("Could not add rtsp session");
        return false;
    }
    
    if (!session->initiateSession(receiver)){
        utils::errorMsg("Could not initiate video session");
        return false;
    }
    
    while (session->getScs()->session == NULL && retries <= RETRIES){
        sleep(1);
        retries++;
    }
    
    MediaSubsessionIterator iter(*(session->getScs()->session));
    MediaSubsession* subsession;
    
    while(iter.next() == NULL && retries <= RETRIES){
        sleep(1);
        retries++;
    }
    
    if (retries > RETRIES){
        delete receiver;
        return false;
    }
    
    utils::infoMsg("RTSP client session created!");
    
    iter.reset();
    
    while((subsession = iter.next()) != NULL){            
        medium = subsession->mediumName();
        
        if (medium.compare("video") == 0){
            addVideoPath(subsession->clientPortNum());
        } else if (medium.compare("audio") == 0){
            addAudioPath(subsession->clientPortNum());
        }
    }
    
    return true;
}

void addConnections(std::vector<int> readers, std::string ip, unsigned port)
{
    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    SinkManager *transmitter = pipe->getTransmitter();
    if (transmitter->addRTPConnection(readers, rand(), ip, port, MPEGTS)) {
        utils::infoMsg("added connection for " + ip + ":" + std::to_string(port));
    }
}

int main(int argc, char* argv[])
{
    std::vector<int> readers;

    int vPort = 0;
    int aPort = 0;
    int port = 0;
    int fps = FRAME_RATE;
    std::string ip;
    std::string sessionId;
    std::string rtspUri;

    utils::setLogLevel(INFO);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i],"-v")==0) {
            vPort = std::stoi(argv[i+1]);
            utils::infoMsg("video input port: " + std::to_string(vPort));
        } else if (strcmp(argv[i],"-a")==0) {
            aPort = std::stoi(argv[i+1]);
            utils::infoMsg("audio input port: " + std::to_string(aPort));
        } else if (strcmp(argv[i],"-d")==0) {
            ip = argv[i + 1];
            utils::infoMsg("destination IP: " + ip);
        } else if (strcmp(argv[i],"-P")==0) {
            port = std::stoi(argv[i+1]);
            utils::infoMsg("destination port: " + std::to_string(port));
        } else if (strcmp(argv[i],"-f")==0) {
            fps = std::stoi(argv[i+1]);
            utils::infoMsg("output frame rate: " + std::to_string(fps));
        } else if (strcmp(argv[i],"-r")==0) {
            rtspUri = argv[i+1];
            utils::infoMsg("output frame rate: " + std::to_string(fps));
        }
    }

    if (vPort == 0 && aPort == 0 && rtspUri.length() == 0){
        utils::errorMsg("invalid parameters");
        return 1;
    }

    PipelineManager *pipe = Controller::getInstance()->pipelineManager();
    if (! pipe->start()){
        utils::errorMsg("Couldn't start pipe");
        return 1;
    }
    SinkManager *transmitter = pipe->getTransmitter();

    signal(SIGINT, signalHandler);

    if (vPort != 0 && rtspUri.length() == 0){
        addVideoSDPSession(vPort);
        addVideoPath(vPort);
    }

    if (aPort != 0 && rtspUri.length() == 0){
        addAudioSDPSession(aPort);
        addAudioPath(aPort);
    }
    
    if (rtspUri.length() > 0){
        if (!addRTSPsession(rtspUri)){
            utils::errorMsg("Couldn't start rtsp client session!");
            return 1;
        }
    }

    for (auto it : pipe->getPaths()) {
        readers.push_back(it.second->getDstReaderID());
    }
  
    sessionId = utils::randomIdGenerator(ID_LENGTH);
    if (!transmitter->addRTSPConnection(readers, 1, STD_RTP, sessionId)){
        return 1;
    }

    sessionId = utils::randomIdGenerator(ID_LENGTH);
    if (!transmitter->addRTSPConnection(readers, 2, MPEGTS, sessionId)){
        return 1;
    }

    if (port != 0 && !ip.empty()){
        addConnections(readers, ip, port);
    }

    while (run) {
        sleep(1);
    }

    return 0;
}
