/*

THIS SOFTWARE IS OPEN SOURCE UNDER THE MIT LICENSE
SUPPORT IS AVAILABE FROM THE AUTHORS


Copyright 2022 Vincent Maciejewski, Quant Enterprises & M2 Tech
Contact:
v@m2te.ch
mayeski@gmail.com
https://www.linkedin.com/in/vmayeski/
http://m2te.ch/


Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

https://opensource.org/licenses/MIT

*/

#pragma once

#include <mutex>
#include <condition_variable>
#include <set>
#include <assert.h>
#include "udp_socket.hpp"
#include "mktdata/SnapshotFullRefreshOrderBook53.h"
#include "mktdata/SnapshotFullRefresh52.h"
#include "mktdata/MDInstrumentDefinitionFuture54.h"
#include "message_buffer.hpp"
#include "mbo_if.hpp"
#include "MessageProcessorIF.hpp"
#include "unistd.h"

namespace m2tech::mdp3
{

    class RecoveryProcessor
    {

    public:
        RecoveryProcessor(
            MessageProcessorIF *_mp,
            feed_handler_if *_cb,
            in_port_t _port_dr,
            in_port_t _port_ir,
            const char *_group_dr,
            const char *_group_ir,
            const char *_interface,
            bool _debug) : mp(_mp),
                           cb(_cb),
                           port_dr(_port_dr),
                           port_ir(_port_ir),
                           group_dr(_group_dr),
                           group_ir(_group_ir),
                           interface(_interface),
                           debug(_debug)

        {
        }

        void do_recovery(bool _recover_instruments)
        {
            std::lock_guard<std::mutex> lk(m);
            recover_instruments = _recover_instruments;
            cv.notify_one();
        }

        void run_recovery() noexcept
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk);
            if (shutdown)
                return;
            if (recover_instruments)
                instrumentrecovery();
            auto last = datarecovery();
            mp->datarecoveryend(last);
        }

        void operator()()
        {
            while (!shutdown)
                run_recovery();
        }

        void end()
        {
            std::lock_guard<std::mutex> lk(m);
            shutdown = true;
            cv.notify_one();
        }

    private:
        std::mutex m;
        std::condition_variable cv;

        bool shutdown = false;

        MessageProcessorIF *mp;
        bool recover_instruments = false;
        bool debug;
        in_port_t port_ir, port_dr;
        int sock_ir, sock_dr;
        struct sockaddr_in addr_ir, addr_dr;
        size_t addrlen_ir, addrlen_dr;
        std::string group_ir, group_dr, interface;

        feed_handler_if *cb;

        std::map<int32_t, std::string> securities;
        std::set<int32_t> recovered_books;

        message_buffer *read_dr() const noexcept
        {
            auto m = new message_buffer();
            auto nrec = mcast::receive(sock_dr, &m->message[0], m2tech::mdp3::msgsz, addr_dr, addrlen_dr, true);
            if (nrec == 0)
            {
                delete m;
                return 0;
            }
            m->len = nrec;
            auto seq_num = *(unsigned int *)(&m->message[0]);
            m->seqnum = seq_num;
            return m;
        }

        message_buffer *read_ir() const noexcept
        {
            auto m = new message_buffer();
            auto nrec = mcast::receive(sock_ir, &m->message[0], m2tech::mdp3::msgsz, addr_ir, addrlen_ir, true);
            if (nrec == 0)
            {
                delete m;
                return 0;
            }
            m->len = nrec;
            auto seq_num = *(unsigned int *)(&m->message[0]);
            m->seqnum = seq_num;
            return m;
        }

        uint32_t datarecovery() noexcept
        {

            if (debug)
                std::cout << "DataRecoveryStart\n";

            mcast::create_udp_socket(port_dr, sock_dr, addr_dr, addrlen_dr);
            mcast::join_group(sock_dr, group_dr.c_str(), interface.c_str());

            uint curr_seq_num = 0;
            bool found_first_packet = false;
            uint32_t last_seq_num = 0;

            uint32_t numreports = std::numeric_limits<uint32_t>::max();

            do
            {
            read_next_packet:
                auto msg = read_dr();
                assert(msg);
                auto databuf = &msg->message[0];
                auto data_start = databuf;

                constexpr size_t sbe_message_header_size = 10;

                auto MsgSeqNum = *(uint32_t *)(databuf);

                //
                // have to wait for msg seq # 1
                //

                if (MsgSeqNum == 1)
                {
                    if (debug)
                        std::cout << "found first packet\n";
                    found_first_packet = true;
                }

                if (!found_first_packet)
                {
                    delete msg;
                    goto read_next_packet;
                }

                if (MsgSeqNum - curr_seq_num > 1 && MsgSeqNum != 1)
                {
                    if (debug)
                        std::cout << "**** packet gap "
                                  << " MsgSeqNum " << MsgSeqNum
                                  << " curr_seq_num " << curr_seq_num
                                  << " numreports " << numreports << std::endl;

                    // have packet gap
                    delete msg;
                    curr_seq_num = 0;
                    found_first_packet = false;
                    numreports = std::numeric_limits<uint32_t>::max();
                    cb->Clear();
                    goto read_next_packet;
                }

                if (debug)
                    std::cout << "processing packet: " << MsgSeqNum << " numreports: " << numreports << std::endl;

                curr_seq_num = MsgSeqNum;

                databuf += sizeof(MsgSeqNum);

                auto SendingTime = *(uint64_t *)(databuf);
                databuf += sizeof(SendingTime);

                while (size_t(databuf - data_start) < msg->len)
                {

                    auto MsgSize = *(uint16_t *)(databuf);
                    databuf += sizeof(MsgSize);

                    auto BlockLength = *(uint16_t *)(databuf);
                    databuf += sizeof(BlockLength);

                    auto template_id = *(uint16_t *)(databuf);
                    databuf += sizeof(template_id);

                    auto SchemaID = *(uint16_t *)(databuf);
                    databuf += sizeof(SchemaID);

                    auto Version = *(uint16_t *)(databuf);
                    databuf += sizeof(Version);

                    databuf -= sbe_message_header_size;

                    if (debug)
                    {
                        std::cout << "datarecovery_handler-----------------------------------------------------------------------------\n";
                        std::cout << "MsgSeqNum " << MsgSeqNum << " MsgSize: " << MsgSize << " template_id: " << template_id << " SchemaID: " << SchemaID << " Version: " << Version << "\n";
                        std::cout << "databuf - data_start " << (databuf - data_start) << " msg->len " << msg->len << std::endl;
                        std::cout << "datarecovery_handler-----------------------------------------------------------------------------\n";
                    }

                    if (template_id == 52)
                    {
                        // mktdata::SnapshotFullRefresh52 rec;
                    }
                    else if (template_id == 53)
                    {
                        mktdata::SnapshotFullRefreshOrderBook53 rec;
                        rec.wrapForDecode(databuf, sbe_message_header_size, BlockLength, Version, MsgSize);

                        // not sure if try/catch needed
                        if (debug)
                        {
                            try
                            {
                                std::cout << rec << std::endl;
                            }
                            catch (...)
                            {
                                std::cout << "EXCEPTION PRINTING SnapshotFullRefreshOrderBook53\n";
                            }
                        }

                        auto tmp_numreports = rec.totNumReports();
                        if (numreports != std::numeric_limits<uint32_t>::max())
                        {
                            if (numreports != tmp_numreports)
                            {
                                if (debug)
                                {
                                    std::cout << "number of reports chnaged " << numreports << " to " << tmp_numreports << std::endl;
                                }
                                delete msg;
                                curr_seq_num = 0;
                                found_first_packet = false;
                                numreports = std::numeric_limits<uint32_t>::max();
                                cb->Clear();
                                goto read_next_packet;
                            }
                        }
                        else
                            numreports = tmp_numreports;
                        auto curr_chunk = rec.currentChunk();
                        auto chunks = rec.noChunks();
                        auto secid = rec.securityID();
                        auto local_last_seq_no = rec.lastMsgSeqNumProcessed();

                        if (debug)
                        {
                            std::cout
                                << "secid " << secid << " "
                                << " local_last_seq_no " << local_last_seq_no
                                << " last_seq_num " << last_seq_num
                                << std::endl;
                        }

                        last_seq_num = std::max(last_seq_num, local_last_seq_no);

                        if (chunks == curr_chunk)
                        {
                            if (debug)
                                std::cout << "recovered book " << secid << std::endl;
                            recovered_books.insert(secid);
                        }

                        if (debug)
                        {
                            std::cout << "secid" << secid << "found chunk " << curr_chunk << " numchunks " << chunks << std::endl;
                            std::cout << "curr_chunk: " << curr_chunk << " curr sec id " << secid
                                      << " last_seq_num " << last_seq_num
                                      << " local_last_seq_no " << local_last_seq_no
                                      << std::endl;
                        }

                        auto txtim = rec.transactTime();
                        auto mdentries = rec.noMDEntries();
                        while (mdentries.hasNext())
                        {
                            mdentries.next();
                            auto dispq = mdentries.mDDisplayQty();
                            auto px_mantissa = mdentries.mDEntryPx().mantissa();
                            auto px_exponent = mdentries.mDEntryPx().exponent();
                            auto mdEntryType = mdentries.mDEntryType();
                            auto order_prio = mdentries.mDOrderPriority();
                            auto order_id = mdentries.orderID();
                            cb->SnapshotFullRefreshOrderBook(
                                MsgSeqNum,
                                txtim,
                                SendingTime,
                                curr_chunk,
                                chunks,
                                secid,
                                dispq,
                                px_mantissa,
                                px_exponent,
                                mdEntryType,
                                order_prio,
                                order_id);
                        }
                    }
                    else
                        abort();

                    databuf += MsgSize;
                }

                delete msg;

                if (debug)
                {
                    std::cout << "reports recovered " << recovered_books.size() << " out of " << securities.size()
                              << " expecting " << numreports
                              << std::endl;
                }

            } while (recovered_books.size() < numreports);

            close(sock_dr);

            return last_seq_num;
        }

        void instrumentrecovery() noexcept
        {

            if (debug)
                std::cout << "InstrumentRecoveryStart\n";

            mcast::create_udp_socket(port_ir, sock_ir, addr_ir, addrlen_ir);
            mcast::join_group(sock_ir, group_ir.c_str(), interface.c_str());

            std::vector<std::string> chunks_processed;
            uint32_t totnumreports;
            std::set<uint32_t> seq_num_processed;

            totnumreports = std::numeric_limits<u_int32_t>::max();

            do
            {
                auto msg = read_ir();
                assert(msg);

                auto databuf = &msg->message[0];
                auto data_start = databuf;

                const size_t sbe_message_header_size = 10;

                auto MsgSeqNum = *(unsigned int *)(databuf);
                databuf += sizeof(MsgSeqNum);

                auto SendingTime = *(unsigned long long *)(databuf);
                databuf += sizeof(SendingTime);

                auto p = seq_num_processed.find(MsgSeqNum);
                if (p != seq_num_processed.end())
                {
                    delete msg;
                    continue;
                }

                seq_num_processed.insert(MsgSeqNum);

                while (size_t(databuf - data_start) < msg->len)
                {

                    auto MsgSize = *(uint16_t *)(databuf);
                    databuf += sizeof(MsgSize);

                    if (MsgSize == 0)
                    {
                        std::cerr << "instrumentrecovery_handler got message size 0\n";
                        abort();
                    }

                    auto BlockLength = *(uint16_t *)(databuf);
                    databuf += sizeof(BlockLength);

                    auto template_id = *(uint16_t *)(databuf);
                    databuf += sizeof(template_id);

                    auto SchemaID = *(uint16_t *)(databuf);
                    databuf += sizeof(SchemaID);

                    auto Version = *(uint16_t *)(databuf);
                    databuf += sizeof(Version);

                    databuf -= sbe_message_header_size;

                    if (debug)
                    {
                        std::cout << "instrumentrecovery_handler-----------------------------------------------------------------------------\n";
                        std::cout << "MsgSize: " << MsgSize << " template_id: " << template_id << " SchemaID: " << SchemaID << " Version: " << Version << "\n";
                        std::cout << "processed: " << (databuf - data_start) << " len: " << msg->len << std::endl;
                        std::cout << "num chunks so far " << chunks_processed.size() << " excpected: " << totnumreports << std::endl;
                        std::cout << "instrumentrecovery_handler-----------------------------------------------------------------------------\n";
                    }

                    if (template_id == 54)
                    {
                        mktdata::MDInstrumentDefinitionFuture54 def;
                        def.wrapForDecode(databuf, sbe_message_header_size, BlockLength, Version, MsgSize);

                        // not sure if try/catch needed
                        if (debug)
                        {
                            try
                            {
                                std::cout << def << std::endl;
                            }
                            catch (...)
                            {
                                std::cout << "EXCEPTION PRINTING MDInstrumentDefinitionFuture54\n";
                            }
                        }

                        auto sym = def.symbol();
                        chunks_processed.push_back(sym);
                        auto asset = def.asset();
                        auto cfiCode = def.cFICode();
                        auto high_limit_px_mantissa = def.highLimitPrice().mantissa();
                        auto high_limit_px_exponent = def.highLimitPrice().exponent();
                        auto low_limit_px_mantissa = def.lowLimitPrice().mantissa();
                        auto low_limit_px_exponent = def.lowLimitPrice().exponent();
                        auto pxvar_mantissa = def.maxPriceVariation().mantissa();
                        auto pxvar_exponent = def.maxPriceVariation().exponent();
                        auto sec_id = def.securityID();
                        auto sec_update_action = def.securityUpdateAction();
                        auto trading_status = def.mDSecurityTradingStatus();
                        auto sec_group = def.securityGroup();
                        auto mkt_seg = def.marketSegmentID();

                        totnumreports = def.totNumReports();

                        uint64_t activation = 0, expiration = 0;
                        auto events = def.noEvents();
                        while (events.hasNext())
                        {
                            events.next();
                            auto evtime = events.eventTime();
                            auto evid = events.eventType();
                            if (evid == 5)
                            {
                                activation = evtime;
                            }
                            else if (evid == 7)
                            {
                                expiration = evtime;
                            }
                            else
                                abort();
                        }

                        cb->MDInstrumentDefinitionFuture(
                            MsgSeqNum,
                            SendingTime,
                            sym,
                            asset,
                            cfiCode,
                            high_limit_px_mantissa,
                            high_limit_px_exponent,
                            low_limit_px_mantissa,
                            low_limit_px_exponent,
                            pxvar_mantissa,
                            pxvar_exponent,
                            sec_id,
                            sec_update_action,
                            trading_status,
                            activation,
                            expiration,
                            sec_group,
                            mkt_seg);

                        securities[sec_id] = std::string(sym);
                    }
                    else if (template_id == 56)
                    {
                        chunks_processed.push_back("spread");
                    }
                    else
                    {
                        std::cerr << "unkonwn template id " << template_id
                                  << std::endl;
                        abort();
                    }

                    databuf += MsgSize;
                }

                delete msg;

            } while (chunks_processed.size() < totnumreports);

            if (debug)
                std::cout << "InstrumentRecoveryEnd\n";

            close(sock_ir);
        }
    };

}