/*
 *  Filter.hh - Filter base classes
 *  Copyright (C) 2014  Fundació i2CAT, Internet i Innovació digital a Catalunya
 *
 *  This file is part of liveMediaStreamer.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Authors:  David Cassany <david.cassany@i2cat.net>,
 *            Marc Palau <marc.palau@i2cat.net>
 *            Gerard Castillo <gerard.castillo@i2cat.net>
 */

#include "Filter.hh"
#include "Utils.hh"

#include <thread>


BaseFilter::BaseFilter(unsigned readersNum, unsigned writersNum, FilterRole fRole_, bool periodic): Runnable(periodic), 
process(false), maxReaders(readersNum), maxWriters(writersNum),  frameTime(std::chrono::microseconds(0)), 
fRole(fRole_), syncTs(std::chrono::microseconds(0))
{
}

BaseFilter::~BaseFilter()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    for (auto it : readers) {
        delete it.second;
    }

    for (auto it : writers) {
        delete it.second;
    }

    readers.clear();
    writers.clear();
    oFrames.clear();
    dFrames.clear();
}

void BaseFilter::setFrameTime(std::chrono::microseconds fTime)
{
    frameTime = fTime;
}

Reader* BaseFilter::getReader(int id)
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    if (readers.count(id) <= 0) {
        return NULL;
    }

    return readers[id];
}

Reader* BaseFilter::setReader(int readerID, FrameQueue* queue)
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    if (readers.size() >= getMaxReaders() || readers.count(readerID) > 0 ) {
        return NULL;
    }

    Reader* r = new Reader();
    readers[readerID] = r;

    return r;
}

int BaseFilter::generateReaderID()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    if (maxReaders == 1) {
        return DEFAULT_ID;
    }

    int id = rand();

    while (readers.count(id) > 0) {
        id = rand();
    }

    return id;
}

int BaseFilter::generateWriterID()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    if (maxWriters == 1) {
        return DEFAULT_ID;
    }

    int id = rand();

    while (writers.count(id) > 0) {
        id = rand();
    }

    return id;
}

bool BaseFilter::demandDestinationFrames()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    if (maxWriters == 0) {
        return true;
    }

    bool newFrame = false;
    for (auto it : writers){
        if (!it.second->isConnected()){
            it.second->disconnect();
            delete it.second;
            writers.erase(it.first);
            continue;
        }

        Frame *f = it.second->getFrame(true);
        f->setConsumed(false);
        dFrames[it.first] = f;
        newFrame = true;
    }

    return newFrame;
}

std::vector<int> BaseFilter::addFrames()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    std::vector<int> enabledJobs;
    
    for (auto it : dFrames){
        if (it.second->getConsumed()) {
            int wId = it.first;
            if (writers[wId]->isConnected()){
                enabledJobs.push_back(writers[wId]->addFrame());
            }
        }
    }
    
    return enabledJobs;
}

std::vector<int> BaseFilter::removeFrames()
{
    std::vector<int> enabledJobs;
    
    if (maxReaders == 0){
        return enabledJobs;
    }
    
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    for (auto it : readers){
        if (oFrames[it.first] && oFrames[it.first]->getConsumed()){
            enabledJobs.push_back(it.second->removeFrame());
        }
    }
    
    return enabledJobs;
}

bool BaseFilter::connect(BaseFilter *R, int writerID, int readerID)
{
    Reader* r;
    FrameQueue *queue = NULL;
    
    std::lock_guard<std::mutex> guard(readersWritersLck);
      
    if (writers.size() >= maxWriters) {
        utils::errorMsg("Too many writers!");
        return false;
    }
    
    if (writers.count(writerID) > 0){
        utils::errorMsg("Id must be unique");
        return false;
    }
    
    writers[writerID] = new Writer();
    seqNums[writerID] = 0;

    if (writers[writerID]->isConnected()) {
        utils::errorMsg("Writer " + std::to_string(writerID) + " null or already connected");
        return false;
    }

    if (R->getReader(readerID) && R->getReader(readerID)->isConnected()){
        utils::errorMsg("Reader " + std::to_string(readerID) + " null or already connected");
        return false;
    }

    queue = allocQueue(getId(), R->getId(), writerID);
    if (!queue){
        return false;
    }

    if (!(r = R->setReader(readerID, queue))) {
        utils::errorMsg("Could not set the queue to the reader");
        return false;
    }

    writers[writerID]->setQueue(queue);

    return writers[writerID]->connect(r);
}

bool BaseFilter::connectOneToOne(BaseFilter *R)
{
    int writerID = generateWriterID();
    int readerID = R->generateReaderID();
    return connect(R, writerID, readerID);
}

bool BaseFilter::connectManyToOne(BaseFilter *R, int writerID)
{
    int readerID = R->generateReaderID();
    return connect(R, writerID, readerID);
}

bool BaseFilter::connectManyToMany(BaseFilter *R, int readerID, int writerID)
{
    return connect(R, writerID, readerID);
}

bool BaseFilter::connectOneToMany(BaseFilter *R, int readerID)
{
    int writerID = generateWriterID();
    return connect(R, writerID, readerID);
}

bool BaseFilter::disconnectWriter(int writerId)
{
    bool ret;
    
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    if (writers.count(writerId) <= 0) {
        return false;
    }

    ret = writers[writerId]->disconnect();
    if (ret){
        writers.erase(writerId);
    }
    return ret;
}

bool BaseFilter::disconnectReader(int readerId)
{
    bool ret;
    
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    if (readers.count(readerId) <= 0) {
        return false;
    }

    ret = readers[readerId]->disconnect();
    if (ret){
        readers.erase(readerId);
    }
    return ret;
}

void BaseFilter::disconnectAll()
{
    std::lock_guard<std::mutex> guard(readersWritersLck);
    
    for (auto it : writers) {
        it.second->disconnect();
    }

    for (auto it : readers) {
        it.second->disconnect();
    }
}

void BaseFilter::processEvent()
{
    std::string action;
    Jzon::Node* params;

    std::lock_guard<std::mutex> guard(eventQueueMutex);

    while(newEvent()) {

        Event e = eventQueue.top();
        action = e.getAction();
        params = e.getParams();

        if (action.empty() || eventMap.count(action) <= 0) {
            utils::errorMsg("Wrong action name while processing event in filter");
            eventQueue.pop();
            continue;
        }

        if (!eventMap[action](params)) {
            utils::errorMsg("Error executing filter event");
        }
        
        eventQueue.pop();
    }
}

bool BaseFilter::newEvent()
{
    if (eventQueue.empty()) {
        return false;
    }

    Event tmp = eventQueue.top();
    if (!tmp.canBeExecuted(std::chrono::system_clock::now())) {
        return false;
    }

    return true;
}

void BaseFilter::pushEvent(Event e)
{
    std::lock_guard<std::mutex> guard(eventQueueMutex);
    eventQueue.push(e);
}

void BaseFilter::getState(Jzon::Object &filterNode)
{
    std::lock_guard<std::mutex> guard(eventQueueMutex);
    filterNode.Add("type", utils::getFilterTypeAsString(fType));
    filterNode.Add("role", utils::getRoleAsString(fRole));
    doGetState(filterNode);
}

std::vector<int> BaseFilter::processFrame(int& ret)
{
    std::vector<int> enabledJobs;

    switch(fRole) {
        case MASTER:
            enabledJobs = masterProcessFrame(ret);
            break;
        case SLAVE:
            enabledJobs = slaveProcessFrame(ret);
            break;
        case NETWORK:
            runDoProcessFrame();
            ret = 0;
            break;
        case SERVER:
            enabledJobs = serverProcessFrame(ret);
        default:
            ret = 0;
            break;
    }

    return enabledJobs;
}

void BaseFilter::processAll()
{
    for (auto it : slaves) {
        it.second->updateFrames(oFrames);
        it.second->execute();
    }
}

bool BaseFilter::runningSlaves()
{
    bool running = false;
    for (auto it : slaves){
        running |= it.second->isProcessing();
    }
    return running;
}

bool BaseFilter::addSlave(BaseFilter *slave)
{
    if (slave->fRole != SLAVE){
        utils::warningMsg("Could not add slave, invalid role");
        return false;
    }
    
    if (slave->getId() < 0){
        utils::warningMsg("Could not add slave, invalid id");
        return false;
    }

    if (slaves.count(slave->getId()) > 0){
        utils::warningMsg("Could not add slave, id must be unique");
        return false;
    }

    slaves[slave->getId()] = slave;

    return true;
}

std::vector<int> BaseFilter::masterProcessFrame(int& ret)
{
    std::chrono::microseconds enlapsedTime;
    std::chrono::microseconds frameTime_;
    std::vector<int> enabledJobs;
    
    processEvent();
      
    if (!demandOriginFrames()) {
        ret = RETRY;
        return enabledJobs;
    }
    
    if (!demandDestinationFrames()) {
        ret = RETRY;
        return enabledJobs;
    }

    processAll();

    runDoProcessFrame();

    while (runningSlaves()){
        std::this_thread::sleep_for(std::chrono::microseconds(RETRY));
    }

    enabledJobs = addFrames();
    removeFrames();

    return enabledJobs;
}

std::vector<int> BaseFilter::serverProcessFrame(int& ret)
{
    std::vector<int> enabledJobs;

    processEvent();
      
    demandOriginFrames();
    demandDestinationFrames();

    runDoProcessFrame();

    enabledJobs = addFrames();
    removeFrames();
    
    ret = 0;
    
    return enabledJobs;
}

std::vector<int> BaseFilter::slaveProcessFrame(int& ret)
{
    std::vector<int> enabledJobs;
    ret = RETRY;

    if (!process) {
        ret = RETRY;
        return enabledJobs;
    }

    processEvent();

    //TODO: decide policy to set run to true/false if retry
    if (!demandDestinationFrames()){
        return enabledJobs;
    }

    runDoProcessFrame();
    
    enabledJobs = addFrames();

    process = false;
    ret = 0;
    
    return enabledJobs;
}

void BaseFilter::updateFrames(std::map<int, Frame*> oFrames_)
{
    oFrames = oFrames_;
}

bool BaseFilter::demandOriginFrames()
{
    bool success;

    if (maxReaders == 0) {
        return true;
    }

    if (readers.empty()) {
        return false;
    }

    if (frameTime.count() <= 0) {
        success = demandOriginFramesBestEffort();
    } else {
        success = demandOriginFramesFrameTime();
    }

    return success;
}

bool BaseFilter::demandOriginFramesBestEffort() 
{
    bool someFrame = false;
    Frame* frame;

    for (auto r : readers) {
        frame = r.second->getFrame();

        while(frame && frame->getPresentationTime() < syncTs) {
            r.second->removeFrame();
            frame = r.second->getFrame();
        }

        if (!frame) {
            frame = r.second->getFrame(true);
            frame->setConsumed(false);
            oFrames[r.first] = frame;
            continue;
        }

        frame->setConsumed(true);
        oFrames[r.first] = frame;
        someFrame = true;
    }

    return someFrame;
}

bool BaseFilter::demandOriginFramesFrameTime() 
{
    Frame* frame;
    std::chrono::microseconds outOfScopeTs = std::chrono::microseconds(-1);
    bool noFrame = true;

    for (auto r : readers) {

        frame = r.second->getFrame();

        // In case a frame is earliear than syncTs, we will discard frames
        // until we found a valid one or until we found a NULL (which will treat later) 
        while(frame && frame->getPresentationTime() < syncTs) {
            r.second->removeFrame();
            frame = r.second->getFrame();
        }

        // If there is no frame get the previous one from the queue
        if (!frame) {
            frame = r.second->getFrame(true);
            frame->setConsumed(false);
            oFrames[r.first] = frame;
            continue;
        }

        // If the current frame is out of our mixing scope, 
        // we get the previous one from the queue
        if (frame->getPresentationTime() >= syncTs + frameTime) {
            frame->setConsumed(false);
            oFrames[r.first] = frame;

            if (outOfScopeTs.count() < 0) {
                outOfScopeTs = frame->getPresentationTime();
            } else {
                outOfScopeTs = std::min(frame->getPresentationTime(), outOfScopeTs);
            }

            continue;
        }

        // Normal case, which means that the frame is in our mixing scope (syncTime -> syncTime+frameTime)
        frame->setConsumed(true);
        oFrames[r.first] = frame;
        noFrame = false;
    }

    // After all, there was no valid frame, so we return false
    // This case is nearly impossible
    if (noFrame) {
        if (outOfScopeTs.count() > 0) {
            syncTs = outOfScopeTs;
        }
        return false;
    } 

    // Finally set syncTs
    syncTs += frameTime;
    return true;
}

OneToOneFilter::OneToOneFilter(FilterRole fRole_, bool periodic) :
    BaseFilter(1, 1, fRole_, periodic)
{
}

bool OneToOneFilter::runDoProcessFrame()
{
    std::chrono::microseconds outTimestamp;

    if (!doProcessFrame(oFrames.begin()->second, dFrames.begin()->second)) {
        return false;
    }

    if (frameTime.count() <= 0) {
        outTimestamp = oFrames.begin()->second->getPresentationTime();
        setSyncTs(outTimestamp);
    } else {
        outTimestamp = getSyncTs();
    }
    
    dFrames.begin()->second->setPresentationTime(outTimestamp);
    dFrames.begin()->second->setDuration(oFrames.begin()->second->getDuration());
    dFrames.begin()->second->setSequenceNumber(oFrames.begin()->second->getSequenceNumber());
    return true;
}


OneToManyFilter::OneToManyFilter(FilterRole fRole_, unsigned writersNum, bool periodic) :
    BaseFilter(1, writersNum, fRole_, periodic)
{
}

bool OneToManyFilter::runDoProcessFrame()
{
    if (!doProcessFrame(oFrames.begin()->second, dFrames)) {
        return false;
    }

	//TODO: implement timestamp setting
    for (auto it : dFrames) {
        // it.second->setPresentationTime(outTimestamp);
        // it.second->setDuration(oFrames.begin()->second->getDuration());
        it.second->setSequenceNumber(oFrames.begin()->second->getSequenceNumber());
    }

    return true;
}


HeadFilter::HeadFilter(FilterRole fRole_, unsigned writersNum, bool periodic) :
    BaseFilter(0, writersNum, fRole_, periodic)
{
}

bool HeadFilter::runDoProcessFrame()
{
    if (!doProcessFrame(dFrames)) {
        return false;
    }

   for (auto it : dFrames) {
       it.second->setSequenceNumber(seqNums[it.first]++);
   }

   return true;
}

void HeadFilter::pushEvent(Event e)
{
    std::string action = e.getAction();
    Jzon::Node* params = e.getParams();
    Jzon::Object outputNode;

    if (action.empty()) {
        return;
    }

    if (eventMap.count(action) <= 0) {
        return;
    }

    if (!eventMap[action](params)) {
        utils::errorMsg("Error executing filter event");
    }
}

TailFilter::TailFilter(FilterRole fRole_, unsigned readersNum, bool periodic) :
    BaseFilter(readersNum, 0, fRole_, periodic)
{
}

bool TailFilter::runDoProcessFrame()
{
    return doProcessFrame(oFrames);
}


void TailFilter::pushEvent(Event e)
{
    std::string action = e.getAction();
    Jzon::Node* params = e.getParams();
    Jzon::Object outputNode;

    if (action.empty()) {
        return;
    }

    if (eventMap.count(action) <= 0) {
        return;
    }

    if (!eventMap[action](params)) {
        utils::errorMsg("Error executing filter event");
    }
}


ManyToOneFilter::ManyToOneFilter(FilterRole fRole_, unsigned readersNum, bool periodic) :
    BaseFilter(readersNum, 1, fRole_, periodic)
{
}

bool ManyToOneFilter::runDoProcessFrame()
{
    if (!doProcessFrame(oFrames, dFrames.begin()->second)) {
        return false;
    }

    //TODO: duration??
    // dFrames.begin()->second->setDuration(oFrames.begin()->second->getDuration());
    seqNums[dFrames.begin()->first]++;
    dFrames.begin()->second->setSequenceNumber(seqNums[dFrames.begin()->first]);
    return true;
}

