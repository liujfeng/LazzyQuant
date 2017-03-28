﻿#include <QSettings>
#include <QDebug>

#include "config_struct.h"
#include "market.h"
#include "utility.h"
#include "multiple_timer.h"
#include "market_watcher.h"
#include "market_watcher_adaptor.h"
#include "tick_receiver.h"

extern QList<Market> markets;

MarketWatcher::MarketWatcher(const CONFIG_ITEM &config, QObject *parent) :
    QObject(parent)
{
    nRequestID = 0;

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, config.organization, config.name);
    QByteArray flowPath = settings.value("FlowPath").toByteArray();
    saveDepthMarketData = settings.value("SaveDepthMarketData").toBool();
    saveDepthMarketDataPath = settings.value("SaveDepthMarketDataPath").toString();
    if (saveDepthMarketDataPath == "") {    // FIXME check if saveDepthMarketDataPath is valid
        saveDepthMarketData = false;
    }

    settings.beginGroup("AccountInfo");
    brokerID = settings.value("BrokerID").toByteArray();
    userID = settings.value("UserID").toByteArray();
    password = settings.value("Password").toByteArray();
    settings.endGroup();

    // Pre-convert QString to char*
    c_brokerID = brokerID.data();
    c_userID = userID.data();
    c_password = password.data();

    settings.beginGroup("SubscribeList");
    QStringList subscribeList = settings.childKeys();
    foreach (const QString &key, subscribeList) {
        if (settings.value(key).toBool()) {
            subscribeSet.insert(key);
        }
    }

    settings.endGroup();

    foreach (const QString &instrumentID, subscribeSet) {
        if (!checkTradingTimes(instrumentID)) {
            qDebug() << instrumentID << "has no proper trading time!";
        }
    }

    pUserApi = CThostFtdcMdApi::CreateFtdcMdApi(flowPath.data());
    pReceiver = new CTickReceiver(this);
    pUserApi->RegisterSpi(pReceiver);

    settings.beginGroup("FrontSites");
    QStringList keys = settings.childKeys();
    const QString protocol = "tcp://";
    foreach (const QString &str, keys) {
        QString address = settings.value(str).toString();
        pUserApi->RegisterFront((protocol + address).toLatin1().data());
    }
    settings.endGroup();

    prepareSaveDepthMarketData();

    new Market_watcherAdaptor(this);
    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.registerObject(config.dbusObject, this);
    dbus.registerService(config.dbusService);

    pUserApi->Init();
}

MarketWatcher::~MarketWatcher()
{
    pUserApi->Release();
    delete pReceiver;
}

void MarketWatcher::prepareSaveDepthMarketData()
{
    for (const QString &instrumentID : subscribeSet) {
        const QString path_for_this_instrumentID = saveDepthMarketDataPath + "/" + instrumentID;
        QDir dir(path_for_this_instrumentID);
        if (!dir.exists()) {
            bool ret = dir.mkpath(path_for_this_instrumentID);
            if (!ret) {
                qDebug() << "Create directory" << path_for_this_instrumentID << "failed!";
            }
        }
    }

    QMap<QTime, QStringList> endPointsMap;
    foreach (const QString &instrumentID, subscribeSet) {
        auto endPoints = getEndPoints(instrumentID);
        foreach (const auto &item, endPoints) {
            endPointsMap[item] << instrumentID;
        }
    }

    auto keys = endPointsMap.keys();
    qSort(keys);
    foreach (const auto &item, keys) {
        instrumentsToSave.append(endPointsMap[item]);
        saveBarTimePoints << item.addSecs(180); // Save data 3 minutes after market close
    }

    auto saveBarTimer = new MultipleTimer(saveBarTimePoints, this);
    connect(saveBarTimer, &MultipleTimer::timesUp, this, &MarketWatcher::saveDepthMarketDataToFile);
}

QDataStream& operator<<(QDataStream& s, const CThostFtdcDepthMarketDataField& dataField)
{
    s.writeRawData((const char*)&dataField, sizeof(CThostFtdcDepthMarketDataField));
    return s;
}

void MarketWatcher::saveDepthMarketDataToFile(int index)
{
    for (const auto &instrumentID : subscribeSet) {
        if (instrumentsToSave[index].contains(instrumentID)) {
            auto &depthMarketDataList = depthMarketDataListMap[instrumentID];
            if (depthMarketDataList.length() > 0) {
                QString file_name = saveDepthMarketDataPath + "/" + instrumentID + "/" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz") + ".data";
                QFile depthMarketDataFile(file_name);
                depthMarketDataFile.open(QFile::WriteOnly);
                QDataStream wstream(&depthMarketDataFile);
                wstream << depthMarketDataList;
                depthMarketDataFile.close();
                depthMarketDataList.clear();
            }
        }
    }
}

void MarketWatcher::customEvent(QEvent *event)
{
    qDebug() << "customEvent: " << int(event->type());
    switch (int(event->type())) {
    case FRONT_CONNECTED:
        login();
        break;
    case FRONT_DISCONNECTED:
    {
        auto *fevent = static_cast<FrontDisconnectedEvent*>(event);
        // TODO
        switch (fevent->getReason()) {
        case 0x1001: // 网络读失败
            break;
        case 0x1002: // 网络写失败
            break;
        case 0x2001: // 接收心跳超时
            break;
        case 0x2002: // 发送心跳失败
            break;
        case 0x2003: // 收到错误报文
            break;
        default:
            break;
        }
    }
        break;
    case HEARTBEAT_WARNING:
    {
        auto *hevent = static_cast<HeartBeatWarningEvent*>(event);
        emit heartBeatWarning(hevent->getLapseTime());
    }
        break;
    case RSP_USER_LOGIN:
        subscribe();
        break;
    case RSP_USER_LOGOUT:
        break;
    case RSP_ERROR:
    case RSP_SUB_MARKETDATA:
    case RSP_UNSUB_MARKETDATA:
        break;
    case DEPTH_MARKET_DATA:
    {
        auto *devent = static_cast<DepthMarketDataEvent*>(event);
        processDepthMarketData(devent->DepthMarketDataField);
    }
        break;
    default:
        QObject::customEvent(event);
        break;
    }
}

/*!
 * \brief MarketWatcher::login
 * 用配置文件中的账号信息登陆行情端
 */
void MarketWatcher::login()
{
    CThostFtdcReqUserLoginField reqUserLogin;
    memset(&reqUserLogin, 0, sizeof (CThostFtdcReqUserLoginField));
    strcpy(reqUserLogin.BrokerID, c_brokerID);
    strcpy(reqUserLogin.UserID, c_userID);
    strcpy(reqUserLogin.Password, c_password);

    pUserApi->ReqUserLogin(&reqUserLogin, nRequestID.fetchAndAddRelaxed(1));
}

/*!
 * \brief MarketWatcher::subscribe
 * 订阅subscribeSet里的合约
 */
void MarketWatcher::subscribe()
{
    const int num = subscribeSet.size();
    char* subscribe_array = new char[num * 32];
    char** ppInstrumentID = new char*[num];
    QSetIterator<QString> iterator(subscribeSet);
    for (int i = 0; i < num; i++) {
        ppInstrumentID[i] = strcpy(subscribe_array + 32 * i, iterator.next().toLatin1().data());
    }

    pUserApi->SubscribeMarketData(ppInstrumentID, num);
    delete[] ppInstrumentID;
    delete[] subscribe_array;
}

/*!
 * \brief MarketWatcher::checkTradingTimes
 * 查找各个交易市场, 找到相应合约的交易时间并事先储存到map里
 *
 * \param instrumentID 合约代码
 * \return 是否找到了该合约的交易时间
 */
bool MarketWatcher::checkTradingTimes(const QString &instrumentID)
{
    const QString instrument = getInstrumentName(instrumentID);
    foreach (const auto &market, markets) {
        foreach (const auto &code, market.codes) {
            if (instrument == code) {
                int i = 0, size = market.regexs.size();
                for (; i < size; i++) {
                    if (QRegExp(market.regexs[i]).exactMatch(instrumentID)) {
                        tradingTimeMap[instrumentID] = market.tradetimeses[i];
                        return true;
                    }
                }
                return false;   // instrumentID未能匹配任何正则表达式
            }
        }
    }
    return false;
}

static inline quint8 charToDigit(const char ten, const char one)
{
    return quint8(10 * (ten - '0') + one - '0');
}

static inline bool isWithinRange(const QTime &t, const QTime &rangeStart, const QTime &rangeEnd)
{
    if (rangeStart < rangeEnd) {
        return rangeStart <= t && t <= rangeEnd;
    } else {
        return rangeStart <= t || t <= rangeEnd;
    }
}

/*!
 * \brief MarketWatcher::processDepthMarketData
 * 处理深度市场数据:
 * 1. 过滤无效的(如在交易时间外的, 或数据有错误的)行情消息
 * 2. 发送新行情数据(newMarketData signal)
 * 3. 如果需要, 将行情数据保存到文件
 *
 * \param depthMarketDataField 深度市场数据
 */
void MarketWatcher::processDepthMarketData(const CThostFtdcDepthMarketDataField& depthMarketDataField)
{
    quint8 hour, minute, second;
    hour   = charToDigit(depthMarketDataField.UpdateTime[0], depthMarketDataField.UpdateTime[1]);
    minute = charToDigit(depthMarketDataField.UpdateTime[3], depthMarketDataField.UpdateTime[4]);
    second = charToDigit(depthMarketDataField.UpdateTime[6], depthMarketDataField.UpdateTime[7]);

    QString instrumentID(depthMarketDataField.InstrumentID);
    QTime time(hour, minute, second);

    foreach (const auto &tradetime, tradingTimeMap[instrumentID]) {
        if (isWithinRange(time, tradetime.first, tradetime.second)) {
            QTime emitTime = (time == tradetime.second) ? time.addSecs(-1) : time;
            emit newMarketData(instrumentID,
                               QTime(0, 0).secsTo(emitTime),
                               depthMarketDataField.LastPrice,
                               depthMarketDataField.Volume,
                               depthMarketDataField.AskPrice1,
                               depthMarketDataField.AskVolume1,
                               depthMarketDataField.BidPrice1,
                               depthMarketDataField.BidVolume1,
                               depthMarketDataField.AskPrice2,
                               depthMarketDataField.AskVolume2,
                               depthMarketDataField.BidPrice2,
                               depthMarketDataField.BidVolume2);

            if (saveDepthMarketData)
                depthMarketDataListMap[instrumentID].append(depthMarketDataField);
            break;
        }
    }
}

/*!
 * \brief MarketWatcher::getTradingDay
 * 获取交易日
 *
 * \return 交易日(YYYYMMDD)
 */
QString MarketWatcher::getTradingDay() const
{
    return pUserApi->GetTradingDay();
}

/*!
 * \brief MarketWatcher::getSubscribeList
 * 获取订阅合约列表
 *
 * \return 订阅合约列表
 */
QStringList MarketWatcher::getSubscribeList() const
{
    return subscribeSet.toList();
}

/*!
 * \brief MarketWatcher::quit
 * 退出
 */
void MarketWatcher::quit()
{
    QCoreApplication::quit();
}
