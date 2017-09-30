#include <algorithm>

#include "socks_server.h"

#include "log.h"

namespace taosocks {

SocksServer::SocksServer(ClientPacketManager& pktmgr, ClientSocket * client)
    : _client(client)
    , _pktmgr(pktmgr)
    , _phrase(Phrase::Init)
{
    _pktmgr.AddHandler(this);

    _client->OnRead([&](ClientSocket*, unsigned char* data, size_t size) {
        feed(data, size);
        if(_phrase == Phrase::Finish) {
            LogLog("�������");
            finish();
        }
    });

    _client->OnWritten([&](ClientSocket*, size_t size) {

    });

    _client->OnClosed([&](ClientSocket*) {

    });
}
void SocksServer::feed(const unsigned char * data, size_t size)
{
    _recv.insert(_recv.cend(), data, data + size);

    auto& D = _recv;

    while(!D.empty()) {
        switch(_phrase) {
        case Phrase::Init:
        {
            _ver = (SocksVersion::Value)D[0];
            D.erase(D.begin());

            if(_ver != SocksVersion::v4) {
                assert(0);
            }

            _phrase = Phrase::Command;
            break;
        }
        case Phrase::Command:
        {
            auto cmd = (SocksCommand::Value)D[0];
            D.erase(D.begin());

            if(cmd != SocksCommand::Stream) {
                assert(0);
            }

            _phrase = Phrase::Port;
            break;
        }
        case Phrase::Port:
        {
            if(D.size() < 2) {
                return;
            }

            unsigned short port_net = D[0] + (D[1] << 8);
            D.erase(D.begin(), D.begin() + 2);
            _port = ::ntohs(port_net);

            _phrase = Phrase::Addr;
            break;
        }
        case Phrase::Addr:
        {
            if(D.size() < 4) {
                return;
            }

            _addr.S_un.S_un_b.s_b1 = D[0];
            _addr.S_un.S_un_b.s_b2 = D[1];
            _addr.S_un.S_un_b.s_b3 = D[2];
            _addr.S_un.S_un_b.s_b4 = D[3];
            D.erase(D.begin(), D.begin() + 4);

            _phrase = Phrase::User;
            break;
        }
        case Phrase::User:
        {
            auto term = std::find_if(D.cbegin(), D.cend(), [](const unsigned char& c) {
                return c == '\0';
            });

            if(term == D.cend()) {
                return;
            }

            D.erase(D.begin(), term + 1);

            _is_v4a = _addr.S_un.S_un_b.s_b1 == 0
                && _addr.S_un.S_un_b.s_b2 == 0
                && _addr.S_un.S_un_b.s_b3 == 0
                && _addr.S_un.S_un_b.s_b4 != 0;

            _phrase = _is_v4a ? Phrase::Domain : Phrase::Finish;

            break;
        }
        case Phrase::Domain:
        {
            auto term = std::find_if(D.cbegin(), D.cend(), [](const unsigned char& c) {
                return c == '\0';
            });

            if(term == D.cend()) {
                return;
            }

            _domain = (char*)&D[0];

            D.erase(D.begin(), term + 1);

            _phrase = Phrase::Finish;

            break;
        }
        }
    }
}

void SocksServer::finish()
{
    auto p = new ResolveAndConnectPacket;
    p->__cmd = PacketCommand::Connect;
    p->__size = sizeof(ResolveAndConnectPacket);
    p->__sfd = (int)INVALID_SOCKET;
    p->__cfd = (int)_client->GetDescriptor();

    auto& host = _domain;
    auto service = std::to_string(_port);

    assert(host.size() > 0 && host.size() < _countof(p->host));
    assert(service.size() > 0 && service.size() < _countof(p->service));

    std::strcpy(p->host, host.c_str());
    std::strcpy(p->service, service.c_str());

    _pktmgr.Send(p);
}

void SocksServer::OnPacket(BasePacket* packet)
{
    switch(packet->__cmd)
    {
    case PacketCommand::Connect:
    {
        auto pkt = static_cast<ResolveAndConnectRespondPacket*>(packet);
        OnResolveAndConnectRespondPacket(pkt);
        break;
    }
    default:
        assert(0 && "invalid packet");
        break;
    }
}

void SocksServer::OnResolveAndConnectRespondPacket(ResolveAndConnectRespondPacket* pkt)
{
    if(pkt->status) {
        std::vector<unsigned char> data;
        data.push_back(0x00);
        data.push_back(ConnectionStatus::Success);

        if(_is_v4a) {
            data.push_back(_port >> 8);
            data.push_back(_port & 0xff);

            auto addr = _addr.S_un.S_addr;
            char* a = (char*)&addr;
            data.push_back(a[0]);
            data.push_back(a[1]);
            data.push_back(a[2]);
            data.push_back(a[3]);
        }
        else {
            data.push_back(0);
            data.push_back(0);
            data.push_back(0);
            data.push_back(0);
            data.push_back(0);
            data.push_back(0);
        }

        _client->Write(data.data(), data.size(), nullptr);

        ConnectionInfo info;
        info.sfd = pkt->__sfd;
        info.cfd = pkt->__cfd;
        info.addr = pkt->addr;
        info.port = pkt->port;
        info.client = _client;
        assert(_onSucceeded);
        _pktmgr.RemoveHandler(this);
        _onSucceeded(info);
    }
    else {
        ConnectionInfo info;
        assert(_onFailed);
        _onFailed(info);
    }
}

}

