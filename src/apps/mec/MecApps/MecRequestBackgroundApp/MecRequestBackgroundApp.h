//
//                           Simu5G
//
// This file is part of a software released under the license included in file
// "license.pdf". This license can be also found at http://www.ltesimulator.com/
// The above file and the present reference are part of the software itself,
// and cannot be removed from it.
//


#ifndef APPS_MEC_MEAPPS_MEBG_APP_H_
#define APPS_MEC_MEAPPS_MEBG_APP_H_

#include "apps/mec/MecApps/MecAppBase.h"
#include "inet/common/lifecycle/NodeStatus.h"

using namespace omnetpp;

class MecRequestBackgroundApp : public MecAppBase
{
protected:

    inet::NodeStatus *nodeStatus = nullptr;
    int numberOfApplications_;    // requests to send in this session
    cMessage *burstTimer;
    cMessage *burstPeriod;
    bool      burstFlag;
    cMessage *sendBurst;

    double lambda; // it is the mean, not the rate

    virtual void handleSelfMessage(cMessage *msg) override;
    virtual int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;

    virtual void handleServiceMessage() override;

    virtual void handleMp1Message() override;

    virtual void handleUeMessage(omnetpp::cMessage *msg) override {};

    virtual void established(int connId) override;

    virtual void sendRequest();

    virtual void finish() override;

   public:
     MecRequestBackgroundApp() {}
     virtual ~MecRequestBackgroundApp();
};

#endif /* APPS_MEC_MEAPPS_MEBGAPP_H_ */
