/*
 * Copyright (c) 2005-2018, BearWare.dk
 *
 * Contact Information:
 *
 * Bjoern D. Rasmussen
 * Kirketoften 5
 * DK-8260 Viby J
 * Denmark
 * Email: contact@bearware.dk
 * Phone: +45 20 20 54 59
 * Web: http://www.bearware.dk
 *
 * This source code is part of the TeamTalk SDK owned by
 * BearWare.dk. Use of this file, or its compiled unit, requires a
 * TeamTalk SDK License Key issued by BearWare.dk.
 *
 * The TeamTalk SDK License Agreement along with its Terms and
 * Conditions are outlined in the file License.txt included with the
 * TeamTalk SDK distribution.
 *
 */

#include "catch.hpp"

#include <iostream>
#include <map>
#include <vector>

#include <pcap/pcap.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <ace/OS.h>
#include <ace/Time_Value.h>

#include <teamtalk/PacketLayout.h>
#include <teamtalk/client/AudioMuxer.h>
#include <codec/WaveFile.h>

#if defined(ENABLE_OPUS)
#include <codec/OpusDecoder.h>
#endif

std::map< ACE_Time_Value, std::vector<char> > GetTTPackets(const ACE_CString& filename,
                                                           const ACE_CString& srcip,
                                                           int srcport,
                                                           const ACE_CString& destip,
                                                           int destport)
{
    std::map< ACE_Time_Value, std::vector<char> > result;
    ACE_Time_Value first;

    char errbuff[PCAP_ERRBUF_SIZE];
    pcap_t * pcap = pcap_open_offline(filename.c_str(), errbuff);
    REQUIRE(pcap);

    struct pcap_pkthdr *header;
    const u_char *data;
    while (int returnValue = pcap_next_ex(pcap, &header, &data) >= 0)
    {
        struct iphdr *iph = (struct iphdr*)(data + sizeof(struct ethhdr));
        struct sockaddr_in src = {}, dest = {};
        src.sin_addr.s_addr = iph->saddr;
        dest.sin_addr.s_addr = iph->daddr;

        if (srcip.length() && srcip != inet_ntoa(src.sin_addr))
            continue;
        if (destip.length() && destip != inet_ntoa(dest.sin_addr))
            continue;

        unsigned short iphdrlen = iph->ihl * 4;
        switch (iph->protocol)
        {
        case 1:  //ICMP Protocol
            break;

        case 2:  //IGMP Protocol
            break;

        case 6:  //TCP Protocol
            break;

        case 17: //UDP Protocol
        {
            struct udphdr *udph = (struct udphdr*)(data + iphdrlen + sizeof(struct ethhdr));
            if (srcport && srcport != ntohs(udph->source))
                continue;
            if (destport && destport != ntohs(udph->dest))
                continue;

            int header_size =  sizeof(struct ethhdr) + iphdrlen + sizeof(struct udphdr);
            auto dataoffset = reinterpret_cast<const char*>(&data[header_size]);
            auto datalen = ntohs(udph->len) - sizeof(struct udphdr);
            std::vector<char> ttraw(dataoffset, dataoffset + datalen);
            ACE_Time_Value tm(ACE_Time_Value(header->ts.tv_sec, header->ts.tv_usec));
            if (first == ACE_Time_Value())
                first = tm;

            tm -= first;
            result[tm] = ttraw;
            break;
        }
        default:
            break;
        }
    }
    pcap_close(pcap);
    return result;
}

#if defined(ENABLE_OPUS)

TEST_CASE("AudioMuxerJitter")
{
    auto ttpackets = GetTTPackets("netem.pcapng", "10.157.1.34", 10333, "10.157.1.40", 63915);

    const int FPP = 2;
    const int SAMPLERATE = 48000, CHANNELS = 2;
    const double FRMDURATION = .120;

    teamtalk::AudioCodec codec;
    codec.codec = teamtalk::CODEC_OPUS;
    codec.opus.samplerate = SAMPLERATE;
    codec.opus.channels = CHANNELS;
    codec.opus.application = OPUS_APPLICATION_VOIP;
    codec.opus.complexity = 10;
    codec.opus.fec = true;
    codec.opus.dtx = true;
    codec.opus.bitrate = 6000;
    codec.opus.vbr = false;
    codec.opus.vbr_constraint = false;
    codec.opus.frame_size = SAMPLERATE * FRMDURATION;
    codec.opus.frames_per_packet = FPP;

    OpusDecode decoder;
    REQUIRE(decoder.Open(SAMPLERATE, CHANNELS));
    std::vector<short> frame(CHANNELS * SAMPLERATE * FRMDURATION * FPP);

    WavePCMFile wavefile;
    REQUIRE(wavefile.NewFile(ACE_TEXT("netem.wav"), SAMPLERATE, CHANNELS));
    uint32_t sampleno = 0;

    AudioMuxer recorder;
    REQUIRE(recorder.SaveFile(codec, ACE_TEXT("netem_muxer.wav"), teamtalk::AFF_WAVE_FORMAT));

    ACE_Time_Value last;
    for (auto i : ttpackets)
    {
        auto data = &i.second[0];
        auto len = i.second.size();
        switch (teamtalk::FieldPacket(data, len).GetKind())
        {
        case teamtalk::PACKET_KIND_VOICE :
        {
            teamtalk::AudioPacket p(data, len);
            REQUIRE(p.ValidatePacket());
            REQUIRE(!p.HasFragments());
            REQUIRE(!p.HasFrameSizes());
            std::cout << "User ID " << p.GetSrcUserID() << " Packet No " << p.GetPacketNumber() << std::endl;
            uint16_t opuslen = 0;
            auto opusenc = p.GetEncodedAudio(opuslen);
            REQUIRE(opusenc);
            int encoffset = 0;
            int enclen = opuslen / FPP;
            int frameoffset = 0;
            
            for (int i=0; i < FPP; ++i)
            {
                int ret = decoder.Decode(&opusenc[encoffset], enclen, &frame[frameoffset * CHANNELS], codec.opus.frame_size);
                REQUIRE(ret == codec.opus.frame_size);
                encoffset += enclen;
                frameoffset += codec.opus.frame_size;
            }
            REQUIRE(frameoffset == FPP * codec.opus.frame_size);
            wavefile.AppendSamples(&frame[0], FPP * codec.opus.frame_size);

            media::AudioFrame frm(media::AudioFormat(SAMPLERATE, CHANNELS),
                                  &frame[0], FPP * codec.opus.frame_size, sampleno);
            REQUIRE(recorder.QueueUserAudio(p.GetSrcUserID(), frm));
            sampleno += FPP * codec.opus.frame_size;
        }
        }
        ACE_Time_Value wait = i.first - last;
        std::cout << "Sleeping " << wait << std::endl;
        ACE_OS::sleep(wait);
        last = i.first;
    }
}

#endif /* ENABLE_OPUS */