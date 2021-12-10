//
//                  Simu5G
//
// Authors: Giovanni Nardini, Giovanni Stea, Antonio Virdis (University of Pisa)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//
#include "corenetwork/gtp/GtpUser.h"
#include "corenetwork/trafficFlowFilter/TftControlInfo_m.h"
#include <iostream>
#include <inet/networklayer/common/L3AddressResolver.h>
#include <inet/networklayer/ipv4/Ipv4Header_m.h>
#include <inet/common/packet/printer/PacketPrinter.h>
#include <inet/common/socket/SocketTag_m.h>
#include <inet/linklayer/common/InterfaceTag_m.h>

Define_Module(GtpUser);

using namespace omnetpp;
using namespace inet;

void GtpUser::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    // wait until all the IP addresses are configured
    if (stage != inet::INITSTAGE_APPLICATION_LAYER)
        return;
    localPort_ = par("localPort");

    // get reference to the binder
    binder_ = getBinder();

    // transport layer access
    socket_.setOutputGate(gate("socketOut"));
    socket_.bind(localPort_);

    tunnelPeerPort_ = par("tunnelPeerPort");

    ownerType_ = selectOwnerType(getAncestorPar("nodeType"));

    // find the address of the core network gateway
    if (ownerType_ != PGW && ownerType_ != UPF)
    {
        // check if this is a gNB connected as secondary node
        bool connectedBS = isBaseStation(ownerType_) && getParentModule()->gate("ppp$o")->isConnected();

        if (connectedBS || ownerType_ == UPF_MEC)
        {
            const char* gateway = getAncestorPar("gateway").stringValue();
            gwAddress_ = L3AddressResolver().resolve(gateway);
        }
    }

    if(isBaseStation(ownerType_))
        myMacNodeID = getParentModule()->par("macNodeId");
    else
        myMacNodeID = 0;

    ie_ = detectInterface();
}

NetworkInterface* GtpUser::detectInterface()
{
    IInterfaceTable *ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
    const char *interfaceName = par("ipOutInterface");
    NetworkInterface *ie = nullptr;

    if (strlen(interfaceName) > 0) {
        ie = ift->findInterfaceByName(interfaceName);
        if (ie == nullptr)
            throw cRuntimeError("Interface \"%s\" does not exist", interfaceName);
    }

    return ie;
}

CoreNodeType GtpUser::selectOwnerType(const char * type)
{
    EV << "GtpUser::selectOwnerType - setting owner type to " << type << endl;
    if(strcmp(type,"ENODEB") == 0)
        return ENB;
    else if(strcmp(type,"GNODEB") == 0)
        return GNB;
    else if(strcmp(type,"PGW") == 0)
        return PGW;
    else if(strcmp(type,"UPF") == 0)
        return UPF;
    else if(strcmp(type, "UPF_MEC") == 0)
        return UPF_MEC;

    error("GtpUser::selectOwnerType - unknown owner type [%s]. Aborting...",type);

    // you should not be here
    return ENB;
}

void GtpUser::handleMessage(cMessage *msg)
{
    if (strcmp(msg->getArrivalGate()->getFullName(), "trafficFlowFilterGate") == 0)
    {
        EV << "GtpUser::handleMessage - message from trafficFlowFilter" << endl;

        // forward the encapsulated Ipv4 datagram
        handleFromTrafficFlowFilter(check_and_cast<Packet *>(msg));
    }
    else if(strcmp(msg->getArrivalGate()->getFullName(),"socketIn")==0)
    {
        EV << "GtpUser::handleMessage - message from udp layer" << endl;
        Packet *packet = check_and_cast<Packet *>(msg);
        PacketPrinter printer; // turns packets into human readable strings
        printer.printPacket(EV, packet); // print to standard output


        handleFromUdp(packet);
    }
}

void GtpUser::handleFromTrafficFlowFilter(Packet * datagram)
{
    auto tftInfo = datagram->removeTag<TftControlInfo>();
    TrafficFlowTemplateId flowId = tftInfo->getTft();

    EV << "GtpUser::handleFromTrafficFlowFilter - Received a tftMessage with flowId[" << flowId << "]" << endl;

    if(flowId == -2)
    {
        // the destination has been removed from the simulation. Delete datagram
        EV << "GtpUser::handleFromTrafficFlowFilter - Destination has been removed from the simulation. Delete packet." << endl;
        delete datagram;
        return;
    }

    // If we are on the eNB and the flowId represents the ID of this eNB, forward the packet locally
    if (flowId == 0)
    {
        // local delivery
        send(datagram,"pppGate");
    }
    else
    {
        // the packet is ready to be tunneled via GTP to another node in the core network
        const auto& hdr = datagram->peekAtFront<Ipv4Header>();
        const Ipv4Address& destAddr = hdr->getDestAddress();

        // create a new GtpUserMessage and encapsulate the datagram within the GtpUserMessage
        auto header = makeShared<GtpUserMsg>();
        header->setTeid(0);
        header->setChunkLength(B(8));
        auto gtpPacket = new Packet(datagram->getName());
        gtpPacket->insertAtFront(header);
        auto data = datagram->peekData();
        gtpPacket->insertAtBack(data);

        delete datagram;

        L3Address tunnelPeerAddress;
        if (flowId == -1) // send to the gateway
        {
            EV << "GtpUser::handleFromTrafficFlowFilter - tunneling to " << gwAddress_.str() << endl;
            tunnelPeerAddress = gwAddress_;
        }
        else if(flowId == -3) // send to a MEC host
        {
            // retrieve the address of the UPF included within the MEC host
            EV << "GtpUser::handleFromTrafficFlowFilter - tunneling to " << destAddr.str() << endl;
            tunnelPeerAddress = binder_->getUpfFromMecHost(inet::L3Address(destAddr));
        }
        else  // send to a BS
        {
            // get the symbolic IP address of the tunnel destination ID
            // then obtain the address via IPvXAddressResolver
            const char* symbolicName = binder_->getModuleNameByMacNodeId(flowId);
            EV << "GtpUser::handleFromTrafficFlowFilter - tunneling to " << symbolicName << endl;
            tunnelPeerAddress = L3AddressResolver().resolve(symbolicName);
        }
        socket_.sendTo(gtpPacket, tunnelPeerAddress, tunnelPeerPort_);
    }
}

void GtpUser::handleFromUdp(Packet * pkt)
{
    EV << "GtpUser::handleFromUdp - Decapsulating and forwarding to the correct destination" << endl;

    // re-create the original IP datagram and send it to the local network
    auto originalPacket = new Packet (pkt->getName());
    auto gtpUserMsg = pkt->popAtFront<GtpUserMsg>();
    originalPacket->insertAtBack(pkt->peekData());
    originalPacket->addTagIfAbsent<PacketProtocolTag>()->setProtocol(&Protocol::ipv4);
    // remove any pending socket indications
    auto sockInd = pkt->removeTagIfPresent<SocketInd>();

    delete pkt;

    L3Address tunnelPeerAddress;

    const auto& hdr = originalPacket->peekAtFront<Ipv4Header>();
    const Ipv4Address& destAddr = hdr->getDestAddress();
    MacNodeId destId = binder_->getMacNodeId(destAddr);
    if (destId != 0)  // final destination is a UE
    {
        MacNodeId destMaster = binder_->getNextHop(destId);

        //checking if serving eNodeB it's --> send to the Radio-NIC
        if(myMacNodeID == destMaster)
        {
            // add Interface-Request for cellular NIC
           if (ie_ != nullptr)
               originalPacket->addTagIfAbsent<InterfaceReq>()->setInterfaceId(ie_->getInterfaceId());

            EV << "GtpUser::handleFromUdp - Datagram local delivery to " << destAddr.str() << endl;
            // local delivery
            send(originalPacket,"pppGate");
            return;
        }

        const char* symbolicName = binder_->getModuleNameByMacNodeId(destMaster);
        tunnelPeerAddress = L3AddressResolver().resolve(symbolicName);
        EV << "GtpUser::handleFromUdp - tunneling to BS " << symbolicName << endl;
    }
    else
    {
        // destination is not a UE

        // check if the destination is a MEC host
        if(binder_->isMecHost(destAddr))
        {
            if (ownerType_== UPF_MEC)
            {
                // we are on the MEC, local delivery
                EV << "GtpUser::handleFromUdp - Datagram local delivery to " << destAddr.str() << endl;
                send(originalPacket,"pppGate");
                return;
            }

            //tunneling to the MEC Host's UPF
            tunnelPeerAddress = binder_->getUpfFromMecHost(destAddr);
            EV << "GtpUser::handleFromUdp - Datagram for " << destAddr.str() << ": tunneling to " << tunnelPeerAddress.str() << endl;
        }
        else
        {
            if (ownerType_== PGW || ownerType_ == UPF)
            {
                // destination is outside the radio network
                send(originalPacket,"pppGate");
                return;
            }
            //tunneling to the PGW/UPF
            EV << "GtpUser::handleFromUdp - Datagram for " << destAddr.str() << ": tunneling to the CN gateway " << gwAddress_.str() << endl;
            tunnelPeerAddress = gwAddress_;
        }
    }


    // send the message to the another core network element through GTP tunneling
    // * create a new GtpUserMessage
    // * encapsulate the datagram within the GtpUserMsg
    auto header = makeShared<GtpUserMsg>();
    header->setTeid(0);
    header->setChunkLength(B(8));
    auto gtpMsg = new Packet(originalPacket->getName());
    gtpMsg->insertAtFront(header);
    auto data = originalPacket->peekData();
    gtpMsg->insertAtBack(data);
    delete originalPacket;

    // create a new GtpUserMessage
    socket_.sendTo(gtpMsg, tunnelPeerAddress, tunnelPeerPort_);
}
