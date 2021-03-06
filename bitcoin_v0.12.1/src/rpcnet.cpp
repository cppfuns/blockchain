// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcserver.h"

#include "chainparams.h"
#include "clientversion.h"
#include "main.h"
#include "net.h"
#include "netbase.h"
#include "protocol.h"
#include "sync.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util.h"
#include "utilstrencodings.h"
#include "version.h"

#include <boost/foreach.hpp>

#include <univalue.h>

using namespace std;

UniValue getconnectioncount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
            "getconnectioncount\n"
            "\nReturns the number of connections to other nodes.\n"
            "\nResult:\n"
            "n          (numeric) The connection count\n"
            "\nExamples:\n"
            + HelpExampleCli("getconnectioncount", "")
            + HelpExampleRpc("getconnectioncount", "")
        );

    LOCK2(cs_main, cs_vNodes);

    return (int)vNodes.size(); // 返回已建立连接的节点列表的大小
}

UniValue ping(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
            "ping\n"
            "\nRequests that a ping be sent to all other nodes, to measure ping time.\n"
            "Results provided in getpeerinfo, pingtime and pingwait fields are decimal seconds.\n"
            "Ping command is handled in queue with all other commands, so it measures processing backlog, not just network ping.\n"
            "\nExamples:\n"
            + HelpExampleCli("ping", "")
            + HelpExampleRpc("ping", "")
        );

    // Request that each node send a ping during next message processing pass
    LOCK2(cs_main, cs_vNodes); // 请求在下一条消息处理完后每个节点发送一个 ping

    BOOST_FOREACH(CNode* pNode, vNodes) { // 遍历已建立连接的每个节点
        pNode->fPingQueued = true; // 设置其 ping 请求队列标志为 true
    }

    return NullUniValue;
}

static void CopyNodeStats(std::vector<CNodeStats>& vstats)
{
    vstats.clear(); // 清空

    LOCK(cs_vNodes); // 上锁
    vstats.reserve(vNodes.size()); // 与开辟空间，防止自动扩容
    BOOST_FOREACH(CNode* pnode, vNodes) { // 遍历以建立连接的节点列表
        CNodeStats stats;
        pnode->copyStats(stats); // 获取节点状态到 stats
        vstats.push_back(stats); // 加入状态列表
    }
}

UniValue getpeerinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令参数反馈
            "getpeerinfo\n"
            "\nReturns data about each connected network node as a json array of objects.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"id\": n,                   (numeric) Peer index\n"
            "    \"addr\":\"host:port\",      (string) The ip address and port of the peer\n"
            "    \"addrlocal\":\"ip:port\",   (string) local address\n"
            "    \"services\":\"xxxxxxxxxxxxxxxx\",   (string) The services offered\n"
            "    \"relaytxes\":true|false,    (boolean) Whether peer has asked us to relay transactions to it\n"
            "    \"lastsend\": ttt,           (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last send\n"
            "    \"lastrecv\": ttt,           (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last receive\n"
            "    \"bytessent\": n,            (numeric) The total bytes sent\n"
            "    \"bytesrecv\": n,            (numeric) The total bytes received\n"
            "    \"conntime\": ttt,           (numeric) The connection time in seconds since epoch (Jan 1 1970 GMT)\n"
            "    \"timeoffset\": ttt,         (numeric) The time offset in seconds\n"
            "    \"pingtime\": n,             (numeric) ping time\n"
            "    \"minping\": n,              (numeric) minimum observed ping time\n"
            "    \"pingwait\": n,             (numeric) ping wait\n"
            "    \"version\": v,              (numeric) The peer version, such as 7001\n"
            "    \"subver\": \"/Satoshi:0.8.5/\",  (string) The string version\n"
            "    \"inbound\": true|false,     (boolean) Inbound (true) or Outbound (false)\n"
            "    \"startingheight\": n,       (numeric) The starting height (block) of the peer\n"
            "    \"banscore\": n,             (numeric) The ban score\n"
            "    \"synced_headers\": n,       (numeric) The last header we have in common with this peer\n"
            "    \"synced_blocks\": n,        (numeric) The last block we have in common with this peer\n"
            "    \"inflight\": [\n"
            "       n,                        (numeric) The heights of blocks we're currently asking from this peer\n"
            "       ...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getpeerinfo", "")
            + HelpExampleRpc("getpeerinfo", "")
        );

    LOCK(cs_main);

    vector<CNodeStats> vstats; // 节点状态列表
    CopyNodeStats(vstats); // 复制节点状态到 vstats

    UniValue ret(UniValue::VARR); // 创建数组类型的结果对象

    BOOST_FOREACH(const CNodeStats& stats, vstats) { // 遍历节点状态列表
        UniValue obj(UniValue::VOBJ);
        CNodeStateStats statestats;
        bool fStateStats = GetNodeStateStats(stats.nodeid, statestats);
        obj.push_back(Pair("id", stats.nodeid)); // 节点 id
        obj.push_back(Pair("addr", stats.addrName)); // 节点地址
        if (!(stats.addrLocal.empty()))
            obj.push_back(Pair("addrlocal", stats.addrLocal)); // 本地地址
        obj.push_back(Pair("services", strprintf("%016x", stats.nServices)));
        obj.push_back(Pair("relaytxes", stats.fRelayTxes));
        obj.push_back(Pair("lastsend", stats.nLastSend));
        obj.push_back(Pair("lastrecv", stats.nLastRecv));
        obj.push_back(Pair("bytessent", stats.nSendBytes));
        obj.push_back(Pair("bytesrecv", stats.nRecvBytes));
        obj.push_back(Pair("conntime", stats.nTimeConnected)); // 建立连接的时间
        obj.push_back(Pair("timeoffset", stats.nTimeOffset));
        obj.push_back(Pair("pingtime", stats.dPingTime)); // ping 时间
        obj.push_back(Pair("minping", stats.dPingMin)); // 最小 ping 时间
        if (stats.dPingWait > 0.0)
            obj.push_back(Pair("pingwait", stats.dPingWait)); // ping 等待时间
        obj.push_back(Pair("version", stats.nVersion)); // 版本号
        // Use the sanitized form of subver here, to avoid tricksy remote peers from
        // corrupting or modifiying the JSON output by putting special characters in
        // their ver message.
        obj.push_back(Pair("subver", stats.cleanSubVer));
        obj.push_back(Pair("inbound", stats.fInbound));
        obj.push_back(Pair("startingheight", stats.nStartingHeight));
        if (fStateStats) {
            obj.push_back(Pair("banscore", statestats.nMisbehavior));
            obj.push_back(Pair("synced_headers", statestats.nSyncHeight)); // 已同步的区块头数
            obj.push_back(Pair("synced_blocks", statestats.nCommonHeight)); // 已同步的区块数
            UniValue heights(UniValue::VARR);
            BOOST_FOREACH(int height, statestats.vHeightInFlight) {
                heights.push_back(height);
            }
            obj.push_back(Pair("inflight", heights));
        }
        obj.push_back(Pair("whitelisted", stats.fWhitelisted));

        ret.push_back(obj);
    }

    return ret;
}

UniValue addnode(const UniValue& params, bool fHelp)
{
    string strCommand;
    if (params.size() == 2) // 若有 2 个参数
        strCommand = params[1].get_str(); // 获取第二个参数作为命令
    if (fHelp || params.size() != 2 ||
        (strCommand != "onetry" && strCommand != "add" && strCommand != "remove")) // 参数必须为 2 个，命令必须为 "onetry" "add" "remove" 这 3 个中的一个
        throw runtime_error( // 命令帮助反馈
            "addnode \"node\" \"add|remove|onetry\"\n"
            "\nAttempts add or remove a node from the addnode list.\n"
            "Or try a connection to a node once.\n"
            "\nArguments:\n"
            "1. \"node\"     (string, required) The node (see getpeerinfo for nodes)\n"
            "2. \"command\"  (string, required) 'add' to add a node to the list, 'remove' to remove a node from the list, 'onetry' to try a connection to the node once\n"
            "\nExamples:\n"
            + HelpExampleCli("addnode", "\"192.168.0.6:8333\" \"onetry\"")
            + HelpExampleRpc("addnode", "\"192.168.0.6:8333\", \"onetry\"")
        );

    string strNode = params[0].get_str(); // 获取指定节点

    if (strCommand == "onetry") // 尝试连接操作
    {
        CAddress addr;
        OpenNetworkConnection(addr, NULL, strNode.c_str()); // 尝试连接一次
        return NullUniValue;
    }

    LOCK(cs_vAddedNodes);
    vector<string>::iterator it = vAddedNodes.begin();
    for(; it != vAddedNodes.end(); it++) // 检查列表中是否存在该节点，若存在，则迭代器指向该节点，否则，指向列表末尾
        if (strNode == *it)
            break;

    if (strCommand == "add") // 添加节点操作
    {
        if (it != vAddedNodes.end()) // 若列表中已存在该节点
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Node already added");
        vAddedNodes.push_back(strNode); // 添加该节点到列表
    }
    else if(strCommand == "remove") // 移除节点操作
    {
        if (it == vAddedNodes.end()) // 若列表中不存在该节点
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
        vAddedNodes.erase(it); // 从列表中擦除该节点
    }

    return NullUniValue; // 无返回值
}

UniValue disconnectnode(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1) // 参数必须为 1 个
        throw runtime_error( // 命令帮助反馈
            "disconnectnode \"node\" \n"
            "\nImmediately disconnects from the specified node.\n"
            "\nArguments:\n"
            "1. \"node\"     (string, required) The node (see getpeerinfo for nodes)\n"
            "\nExamples:\n"
            + HelpExampleCli("disconnectnode", "\"192.168.0.6:8333\"")
            + HelpExampleRpc("disconnectnode", "\"192.168.0.6:8333\"")
        );

    CNode* pNode = FindNode(params[0].get_str()); // 查找指定节点
    if (pNode == NULL) // 未找到要断开连接的节点
        throw JSONRPCError(RPC_CLIENT_NODE_NOT_CONNECTED, "Node not found in connected nodes");

    pNode->fDisconnect = true; // 标记该节点的断开连接标志为 true

    return NullUniValue;
}

UniValue getaddednodeinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2) // 参数至少为 1 个，至多为 2 个
        throw runtime_error( // 命令帮助反馈
            "getaddednodeinfo dns ( \"node\" )\n"
            "\nReturns information about the given added node, or all added nodes\n"
            "(note that onetry addnodes are not listed here)\n"
            "If dns is false, only a list of added nodes will be provided,\n"
            "otherwise connected information will also be available.\n"
            "\nArguments:\n"
            "1. dns        (boolean, required) If false, only a list of added nodes will be provided, otherwise connected information will also be available.\n"
            "2. \"node\"   (string, optional) If provided, return information about this specific node, otherwise all nodes are returned.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"addednode\" : \"192.168.0.201\",   (string) The node ip address\n"
            "    \"connected\" : true|false,          (boolean) If connected\n"
            "    \"addresses\" : [\n"
            "       {\n"
            "         \"address\" : \"192.168.0.201:8333\",  (string) The bitcoin server host and port\n"
            "         \"connected\" : \"outbound\"           (string) connection, inbound or outbound\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getaddednodeinfo", "true")
            + HelpExampleCli("getaddednodeinfo", "true \"192.168.0.201\"")
            + HelpExampleRpc("getaddednodeinfo", "true, \"192.168.0.201\"")
        );

    bool fDns = params[0].get_bool(); // 获取 dns 标志

    list<string> laddedNodes(0); // 添加节点 IP 的双向环状链表
    if (params.size() == 1) // 只有一个参数，未指定节点
    {
        LOCK(cs_vAddedNodes);
        BOOST_FOREACH(const std::string& strAddNode, vAddedNodes) // 遍历添加节点 IP 的列表
            laddedNodes.push_back(strAddNode); // 依次添加到该双向环状链表
    }
    else
    { // 超过 1 个参数
        string strNode = params[1].get_str(); // 获取指定节点 IP 的字符串
        LOCK(cs_vAddedNodes);
        BOOST_FOREACH(const std::string& strAddNode, vAddedNodes) { // 遍历添加节点 IP 的列表
            if (strAddNode == strNode) // 若指定了节点
            {
                laddedNodes.push_back(strAddNode); // 添加到双向环状链表
                break; // 跳出
            }
        }
        if (laddedNodes.size() == 0) // 若该链表大小为 0，表示没有节点被添加
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
    }

    UniValue ret(UniValue::VARR); // 创建数组类型的结果对象
    if (!fDns) // 若关闭了 dns
    {
        BOOST_FOREACH (const std::string& strAddNode, laddedNodes) { // 遍历添加节点 IP 的列表
            UniValue obj(UniValue::VOBJ);
            obj.push_back(Pair("addednode", strAddNode));
            ret.push_back(obj); // 加入结果对象
        }
        return ret; // 返回结果
    } // 若开启了 dns

    list<pair<string, vector<CService> > > laddedAddreses(0); // 添加地址的双向环状链表
    BOOST_FOREACH(const std::string& strAddNode, laddedNodes) { // 遍历添加节点的链表
        vector<CService> vservNode(0);
        if(Lookup(strAddNode.c_str(), vservNode, Params().GetDefaultPort(), fNameLookup, 0)) // IP + 端口 获取服务节点
            laddedAddreses.push_back(make_pair(strAddNode, vservNode)); // 追加到添加地址的双向环状链表
        else
        {
            UniValue obj(UniValue::VOBJ);
            obj.push_back(Pair("addednode", strAddNode));
            obj.push_back(Pair("connected", false));
            UniValue addresses(UniValue::VARR);
            obj.push_back(Pair("addresses", addresses));
        }
    }

    LOCK(cs_vNodes);
    for (list<pair<string, vector<CService> > >::iterator it = laddedAddreses.begin(); it != laddedAddreses.end(); it++) // 遍历添加地址的双向环状链表
    {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("addednode", it->first));

        UniValue addresses(UniValue::VARR);
        bool fConnected = false; // 连接标志
        BOOST_FOREACH(const CService& addrNode, it->second) {
            bool fFound = false; // 是否在连接的节点列表中找到
            UniValue node(UniValue::VOBJ);
            node.push_back(Pair("address", addrNode.ToString()));
            BOOST_FOREACH(CNode* pnode, vNodes) { // 遍历已建立连接的节点列表
                if (pnode->addr == addrNode) // 若该节点已建立连接
                {
                    fFound = true;
                    fConnected = true;
                    node.push_back(Pair("connected", pnode->fInbound ? "inbound" : "outbound")); // 追加该节点的连接状态
                    break;
                }
            }
            if (!fFound) // 未找到，未连接
                node.push_back(Pair("connected", "false"));
            addresses.push_back(node);
        }
        obj.push_back(Pair("connected", fConnected));
        obj.push_back(Pair("addresses", addresses));
        ret.push_back(obj); // 加入结果集
    }

    return ret; // 返回数组类型的结果
}

UniValue getnettotals(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
            "getnettotals\n"
            "\nReturns information about network traffic, including bytes in, bytes out,\n"
            "and current time.\n"
            "\nResult:\n"
            "{\n"
            "  \"totalbytesrecv\": n,   (numeric) Total bytes received\n"
            "  \"totalbytessent\": n,   (numeric) Total bytes sent\n"
            "  \"timemillis\": t,       (numeric) Total cpu time\n"
            "  \"uploadtarget\":\n"
            "  {\n"
            "    \"timeframe\": n,                         (numeric) Length of the measuring timeframe in seconds\n"
            "    \"target\": n,                            (numeric) Target in bytes\n"
            "    \"target_reached\": true|false,           (boolean) True if target is reached\n"
            "    \"serve_historical_blocks\": true|false,  (boolean) True if serving historical blocks\n"
            "    \"bytes_left_in_cycle\": t,               (numeric) Bytes left in current time cycle\n"
            "    \"time_left_in_cycle\": t                 (numeric) Seconds left in current time cycle\n"
            "  }\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getnettotals", "")
            + HelpExampleRpc("getnettotals", "")
       );

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("totalbytesrecv", CNode::GetTotalBytesRecv())); // 接收的总字节数
    obj.push_back(Pair("totalbytessent", CNode::GetTotalBytesSent())); // 发送的总字节数
    obj.push_back(Pair("timemillis", GetTimeMillis())); // 时间毫秒

    UniValue outboundLimit(UniValue::VOBJ);
    outboundLimit.push_back(Pair("timeframe", CNode::GetMaxOutboundTimeframe()));
    outboundLimit.push_back(Pair("target", CNode::GetMaxOutboundTarget()));
    outboundLimit.push_back(Pair("target_reached", CNode::OutboundTargetReached(false)));
    outboundLimit.push_back(Pair("serve_historical_blocks", !CNode::OutboundTargetReached(true)));
    outboundLimit.push_back(Pair("bytes_left_in_cycle", CNode::GetOutboundTargetBytesLeft()));
    outboundLimit.push_back(Pair("time_left_in_cycle", CNode::GetMaxOutboundTimeLeftInCycle()));
    obj.push_back(Pair("uploadtarget", outboundLimit));
    return obj;
}

static UniValue GetNetworksInfo()
{
    UniValue networks(UniValue::VARR);
    for(int n=0; n<NET_MAX; ++n)
    { // 遍历所有网络类型
        enum Network network = static_cast<enum Network>(n); // 强制类型转换为枚举 Network
        if(network == NET_UNROUTABLE) // != 0
            continue;
        proxyType proxy;
        UniValue obj(UniValue::VOBJ);
        GetProxy(network, proxy);
        obj.push_back(Pair("name", GetNetworkName(network))); // 网络名
        obj.push_back(Pair("limited", IsLimited(network))); // 是否受限
        obj.push_back(Pair("reachable", IsReachable(network))); // 是否可接入
        obj.push_back(Pair("proxy", proxy.IsValid() ? proxy.proxy.ToStringIPPort() : string())); // 代理
        obj.push_back(Pair("proxy_randomize_credentials", proxy.randomize_credentials)); // 代理随机化证书
        networks.push_back(obj);
    }
    return networks;
}

UniValue getnetworkinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
            "getnetworkinfo\n"
            "Returns an object containing various state info regarding P2P networking.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,                      (numeric) the server version\n"
            "  \"subversion\": \"/Satoshi:x.x.x/\",     (string) the server subversion string\n"
            "  \"protocolversion\": xxxxx,              (numeric) the protocol version\n"
            "  \"localservices\": \"xxxxxxxxxxxxxxxx\", (string) the services we offer to the network\n"
            "  \"timeoffset\": xxxxx,                   (numeric) the time offset\n"
            "  \"connections\": xxxxx,                  (numeric) the number of connections\n"
            "  \"networks\": [                          (array) information per network\n"
            "  {\n"
            "    \"name\": \"xxx\",                     (string) network (ipv4, ipv6 or onion)\n"
            "    \"limited\": true|false,               (boolean) is the network limited using -onlynet?\n"
            "    \"reachable\": true|false,             (boolean) is the network reachable?\n"
            "    \"proxy\": \"host:port\"               (string) the proxy that is used for this network, or empty if none\n"
            "  }\n"
            "  ,...\n"
            "  ],\n"
            "  \"relayfee\": x.xxxxxxxx,                (numeric) minimum relay fee for non-free transactions in " + CURRENCY_UNIT + "/kB\n"
            "  \"localaddresses\": [                    (array) list of local addresses\n"
            "  {\n"
            "    \"address\": \"xxxx\",                 (string) network address\n"
            "    \"port\": xxx,                         (numeric) network port\n"
            "    \"score\": xxx                         (numeric) relative score\n"
            "  }\n"
            "  ,...\n"
            "  ]\n"
            "  \"warnings\": \"...\"                    (string) any network warnings (such as alert messages) \n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getnetworkinfo", "")
            + HelpExampleRpc("getnetworkinfo", "")
        );

    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ); // 创建一个对象类型的结果对象
    obj.push_back(Pair("version",       CLIENT_VERSION)); // 版本
    obj.push_back(Pair("subversion",    strSubVersion)); // 子版本
    obj.push_back(Pair("protocolversion",PROTOCOL_VERSION)); // 协议版本
    obj.push_back(Pair("localservices",       strprintf("%016x", nLocalServices))); // 本地服务
    obj.push_back(Pair("timeoffset",    GetTimeOffset()));
    obj.push_back(Pair("connections",   (int)vNodes.size())); // 连接数
    obj.push_back(Pair("networks",      GetNetworksInfo())); // 网络信息
    obj.push_back(Pair("relayfee",      ValueFromAmount(::minRelayTxFee.GetFeePerK())));
    UniValue localAddresses(UniValue::VARR); // 数组类型对象
    {
        LOCK(cs_mapLocalHost);
        BOOST_FOREACH(const PAIRTYPE(CNetAddr, LocalServiceInfo) &item, mapLocalHost)
        {
            UniValue rec(UniValue::VOBJ);
            rec.push_back(Pair("address", item.first.ToString())); // 地址
            rec.push_back(Pair("port", item.second.nPort)); // 端口
            rec.push_back(Pair("score", item.second.nScore));
            localAddresses.push_back(rec);
        }
    }
    obj.push_back(Pair("localaddresses", localAddresses)); // 本地地址
    obj.push_back(Pair("warnings",       GetWarnings("statusbar"))); // 警告
    return obj;
}

UniValue setban(const UniValue& params, bool fHelp)
{
    string strCommand;
    if (params.size() >= 2)
        strCommand = params[1].get_str();
    if (fHelp || params.size() < 2 ||
        (strCommand != "add" && strCommand != "remove")) // 参数至少为 2 个
        throw runtime_error( // 命令帮助反馈
                            "setban \"ip(/netmask)\" \"add|remove\" (bantime) (absolute)\n"
                            "\nAttempts add or remove a IP/Subnet from the banned list.\n"
                            "\nArguments:\n"
                            "1. \"ip(/netmask)\" (string, required) The IP/Subnet (see getpeerinfo for nodes ip) with a optional netmask (default is /32 = single ip)\n"
                            "2. \"command\"      (string, required) 'add' to add a IP/Subnet to the list, 'remove' to remove a IP/Subnet from the list\n"
                            "3. \"bantime\"      (numeric, optional) time in seconds how long (or until when if [absolute] is set) the ip is banned (0 or empty means using the default time of 24h which can also be overwritten by the -bantime startup argument)\n"
                            "4. \"absolute\"     (boolean, optional) If set, the bantime must be a absolute timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
                            "\nExamples:\n"
                            + HelpExampleCli("setban", "\"192.168.0.6\" \"add\" 86400")
                            + HelpExampleCli("setban", "\"192.168.0.0/24\" \"add\"")
                            + HelpExampleRpc("setban", "\"192.168.0.6\", \"add\" 86400")
                            );

    CSubNet subNet;
    CNetAddr netAddr;
    bool isSubnet = false; // 子网标志，默认为 false

    if (params[0].get_str().find("/") != string::npos) // 检查指定的 ip 地址中是否包含子网
        isSubnet = true;

    if (!isSubnet) // 无子网
        netAddr = CNetAddr(params[0].get_str());
    else // 含子网
        subNet = CSubNet(params[0].get_str());

    if (! (isSubnet ? subNet.IsValid() : netAddr.IsValid()) ) // 检查网络有效性
        throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Invalid IP/Subnet");

    if (strCommand == "add")
    { // 添加选项
        if (isSubnet ? CNode::IsBanned(subNet) : CNode::IsBanned(netAddr)) // 检查是否已经禁止
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: IP/Subnet already banned");

        int64_t banTime = 0; //use standard bantime if not specified
        if (params.size() >= 3 && !params[2].isNull()) // 若有 3 个参数且第 3 个参数非空
            banTime = params[2].get_int64(); // 获取禁止时间

        bool absolute = false; // 绝对时间，默认关闭
        if (params.size() == 4 && params[3].isTrue()) // 若有 4 个参数且第 4 个参数为 true
            absolute = true; // 绝对时间标志置为 true

        isSubnet ? CNode::Ban(subNet, BanReasonManuallyAdded, banTime, absolute) : CNode::Ban(netAddr, BanReasonManuallyAdded, banTime, absolute); // 禁止指定地址

        //disconnect possible nodes
        while(CNode *bannedNode = (isSubnet ? FindNode(subNet) : FindNode(netAddr))) // 查找节点列表
            bannedNode->fDisconnect = true; // 把存在的节点的断开连接标志置为 true
    }
    else if(strCommand == "remove")
    { // 移除选项
        if (!( isSubnet ? CNode::Unban(subNet) : CNode::Unban(netAddr) )) // 解禁网络
            throw JSONRPCError(RPC_MISC_ERROR, "Error: Unban failed");
    }

    DumpBanlist(); //store banlist to disk
    uiInterface.BannedListChanged();

    return NullUniValue;
}

UniValue listbanned(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
                            "listbanned\n"
                            "\nList all banned IPs/Subnets.\n"
                            "\nExamples:\n"
                            + HelpExampleCli("listbanned", "")
                            + HelpExampleRpc("listbanned", "")
                            );

    banmap_t banMap;
    CNode::GetBanned(banMap); // 获取禁止列表

    UniValue bannedAddresses(UniValue::VARR); // 创建数组类型的禁止地址
    for (banmap_t::iterator it = banMap.begin(); it != banMap.end(); it++)
    { // 遍历禁止列表
        CBanEntry banEntry = (*it).second; // 获取禁止条目
        UniValue rec(UniValue::VOBJ);
        rec.push_back(Pair("address", (*it).first.ToString())); // 子网地址
        rec.push_back(Pair("banned_until", banEntry.nBanUntil)); // 禁止结束时间
        rec.push_back(Pair("ban_created", banEntry.nCreateTime)); // 创建禁止时间
        rec.push_back(Pair("ban_reason", banEntry.banReasonToString())); // 禁止原因

        bannedAddresses.push_back(rec); // 加入结果集
    }

    return bannedAddresses; // 返回禁止列表
}

UniValue clearbanned(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) // 没有参数
        throw runtime_error( // 命令帮助反馈
                            "clearbanned\n"
                            "\nClear all banned IPs.\n"
                            "\nExamples:\n"
                            + HelpExampleCli("clearbanned", "")
                            + HelpExampleRpc("clearbanned", "")
                            );

    CNode::ClearBanned(); // 清空禁止列表
    DumpBanlist(); //store banlist to disk // 导出禁止列表到硬盘
    uiInterface.BannedListChanged();

    return NullUniValue;
}
