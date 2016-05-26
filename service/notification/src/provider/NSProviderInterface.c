//******************************************************************
//
// Copyright 2016 Samsung Electronics All Rights Reserved.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

#include "NSProviderInterface.h"
#include "NSProviderScheduler.h"
#include "NSProviderListener.h"
#include "NSCacheAdapter.h"
#include "NSProviderSubscription.h"
#include "NSProviderNotification.h"
#include "NSProviderMemoryCache.h"
#include "oic_malloc.h"
#include "oic_string.h"
#include "cautilinterface.h"

bool initProvider = false;
static NSSubscribeRequestCallback g_subscribeRequestCb = NULL;
static NSSyncCallback g_syncCb = NULL;

pthread_mutex_t nsInitMutex;

void initializeMutex()
{
    static pthread_mutex_t initMutex = PTHREAD_MUTEX_INITIALIZER;
    nsInitMutex = initMutex;
}

void NSRegisterSubscribeRequestCb(NSSubscribeRequestCallback subscribeRequestCb)
{
    NS_LOG(DEBUG, "NSRegisterSubscribeRequestCb - IN");
    g_subscribeRequestCb = subscribeRequestCb;
    NS_LOG(DEBUG, "NSRegisterSubscribeRequestCb - OUT");
}

void  NSRegisterSyncCb(NSSyncCallback syncCb)
{
    NS_LOG(DEBUG, "NSRegisterSyncCb - IN");
    g_syncCb = syncCb;
    NS_LOG(DEBUG, "NSRegisterSyncCb - OUT");
}

void NSSubscribeRequestCb(NSConsumer *consumer)
{
    NS_LOG(DEBUG, "NSSubscribeRequestCb - IN");
    g_subscribeRequestCb(consumer);
    NS_LOG(DEBUG, "NSSubscribeRequestCb - OUT");
}

void NSSyncCb(NSSync *sync)
{
    NS_LOG(DEBUG, "NSSyncCb - IN");
    g_syncCb(sync);
    NS_LOG(DEBUG, "NSSyncCb - OUT");
}

NSResult NSStartProvider(NSAccessPolicy policy, NSSubscribeRequestCallback subscribeRequestCb,
        NSSyncCallback syncCb)
{
    OIC_LOG(INFO, INTERFACE_TAG, "Notification Service Start Provider..");
    NS_LOG(DEBUG, "NSStartProvider - IN");

    initializeMutex();

    pthread_mutex_lock(&nsInitMutex);

    if (!initProvider)
    {
        NS_LOG(DEBUG, "Init Provider");
        initProvider = true;
        NSSetSubscriptionAcceptPolicy(policy);
        NSRegisterSubscribeRequestCb(subscribeRequestCb);
        NSRegisterSyncCb(syncCb);
        CARegisterNetworkMonitorHandler(NSProviderAdapterStateListener,
                NSProviderConnectionStateListener);

        NSSetList();
        NSInitScheduler();
        NSStartScheduler();

        NSPushQueue(DISCOVERY_SCHEDULER, TASK_START_PRESENCE, NULL);
        NSPushQueue(DISCOVERY_SCHEDULER, TASK_REGISTER_RESOURCE, NULL);
    }
    else
    {
        NS_LOG(DEBUG, "Already started Notification Provider");
    }

    pthread_mutex_unlock(&nsInitMutex);

    NS_LOG(DEBUG, "NSStartProvider - OUT");

    return NS_OK;
}

void NSSetList()
{
    NS_LOG(DEBUG, "NSSetList - IN");
    pthread_mutex_init(&NSCacheMutex, NULL);
    NSInitSubscriptionList();
    NSInitMessageList();
    NS_LOG(DEBUG, "NSSetList - OUT");
}

NSResult NSStopProvider()
{
    NS_LOG(DEBUG, "NSStopProvider - IN");

    pthread_mutex_lock(&nsInitMutex);

    NSRegisterSubscribeRequestCb((NSSubscribeRequestCallback)NULL);
    NSRegisterSyncCb((NSSyncCallback)NULL);
    initProvider = false;

    pthread_mutex_unlock(&nsInitMutex);
    NS_LOG(DEBUG, "NSStopProvider - OUT");
    return NS_OK;
}

NSResult NSSendNotification(NSMessage *msg)
{
    OIC_LOG(INFO, INTERFACE_TAG, "Send Notification");
    NS_LOG(DEBUG, "NSSendNotification - IN");

    NSMessage * newMsg = NSDuplicateMessage(msg);

    if(newMsg == NULL)
    {
        NS_LOG(ERROR, "Msg is NULL");
        return NS_ERROR;
    }

    NSPushQueue(NOTIFICATION_SCHEDULER, TASK_SEND_NOTIFICATION, newMsg);
    NS_LOG(DEBUG, "NSSendNotification - OUT");
    return NS_OK;
}

NSResult NSProviderReadCheck(NSMessage *msg)
{
    OIC_LOG(INFO, INTERFACE_TAG, "Read Sync");
    NS_LOG(DEBUG, "NSProviderReadCheck - IN");
    NSPushQueue(NOTIFICATION_SCHEDULER, TASK_SEND_READ, msg);
    NS_LOG(DEBUG, "NSProviderReadCheck - OUT");
    return NS_OK;
}

NSResult NSAccept(NSConsumer *consumer, bool accepted)
{
    OIC_LOG(INFO, INTERFACE_TAG, "Response Acceptance");
    NS_LOG(DEBUG, "NSAccept - IN");

    if(accepted)
    {
        NS_LOG(DEBUG, "accepted is true - ALLOW");
        NSPushQueue(SUBSCRIPTION_SCHEDULER, TASK_SEND_ALLOW, consumer);
    }
    else
    {
        NS_LOG(DEBUG, "accepted is false - DENY");
        NSPushQueue(SUBSCRIPTION_SCHEDULER, TASK_SEND_DENY, consumer);
    }

    NS_LOG(DEBUG, "NSAccept - OUT");
    return NS_OK;
}

void * NSResponseSchedule(void * ptr)
{
    if (ptr == NULL)
    {
        OIC_LOG(INFO, INTERFACE_TAG, "Init NSResponseSchedule");
        NS_LOG(DEBUG, "Create NSReponseSchedule");
    }

    while (NSIsRunning[RESPONSE_SCHEDULER])
    {
        sem_wait(&NSSemaphore[RESPONSE_SCHEDULER]);
        pthread_mutex_lock(&NSMutex[RESPONSE_SCHEDULER]);

        if (NSHeadMsg[RESPONSE_SCHEDULER] != NULL)
        {
            NSTask *node = NSHeadMsg[RESPONSE_SCHEDULER];
            NSHeadMsg[RESPONSE_SCHEDULER] = node->nextTask;

            switch (node->taskType)
            {
                case TASK_CB_SUBSCRIPTION:
                {
                    NS_LOG(DEBUG, "CASE TASK_CB_SUBSCRIPTION : ");
                    OCEntityHandlerRequest * request = (OCEntityHandlerRequest*)node->taskData;
                    NSConsumer consumer;

                    consumer.mId = OICStrdup(request->devAddr.addr);
                    int * obId = (int *) OICMalloc(sizeof(int));
                    *obId = request->obsInfo.obsId;
                    consumer.mUserData = obId;

                    NSSubscribeRequestCb(&consumer);
                    NSFreeOCEntityHandlerRequest(request);

                    break;
                }
                case TASK_CB_SYNC:
                {
                    NS_LOG(DEBUG, "CASE TASK_CB_SYNC : ");
                    NSSync * sync = (NSSync*)node->taskData;
                    NSSyncCb(sync);
                    break;
                }
                default:
                    OIC_LOG(INFO, INTERFACE_TAG, "Response to User");

                    break;
            }
            OICFree(node);
        }

        pthread_mutex_unlock(&NSMutex[RESPONSE_SCHEDULER]);

    }
    return NULL;
}

NSResult NSTestStartPresence()
{
    NSPushQueue(DISCOVERY_SCHEDULER, TASK_START_PRESENCE, NULL);
    return NS_OK;
}

NSResult NSTestStopPresence()
{
    NSPushQueue(DISCOVERY_SCHEDULER, TASK_STOP_PRESENCE, NULL);
    return NS_OK;
}


