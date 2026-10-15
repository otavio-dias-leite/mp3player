#include "JohnnyCore.h"
#include <string.h>

namespace johnny {

bool ResourceManager::acquire(uint8_t owner, Resource resource) {
  if (!owner) return false;
  const uint8_t i=static_cast<uint8_t>(resource);
  if (i>=kResourceCount) return false;
  portENTER_CRITICAL(&mux_);
  const bool ok=(owners_[i]==0 || owners_[i]==owner);
  if(ok) owners_[i]=owner;
  portEXIT_CRITICAL(&mux_);
  return ok;
}
bool ResourceManager::acquireMask(uint8_t owner,uint32_t mask) {
  if(!owner || (mask>>kResourceCount)!=0) return false;
  portENTER_CRITICAL(&mux_);
  bool ok=true;
  for(uint8_t i=0;i<kResourceCount;i++)
    if((mask&(1UL<<i)) && owners_[i] && owners_[i]!=owner){ok=false;break;}
  if(ok) for(uint8_t i=0;i<kResourceCount;i++) if(mask&(1UL<<i)) owners_[i]=owner;
  portEXIT_CRITICAL(&mux_);
  return ok;
}
void ResourceManager::release(uint8_t owner,Resource resource) {
  const uint8_t i=static_cast<uint8_t>(resource);
  if(!owner || i>=kResourceCount) return;
  portENTER_CRITICAL(&mux_);
  if(owners_[i]==owner) owners_[i]=0;
  portEXIT_CRITICAL(&mux_);
}
void ResourceManager::releaseOwner(uint8_t owner) {
  if(!owner) return;
  portENTER_CRITICAL(&mux_);
  for(uint8_t i=0;i<kResourceCount;i++) if(owners_[i]==owner) owners_[i]=0;
  portEXIT_CRITICAL(&mux_);
}
bool ResourceManager::ownedBy(uint8_t owner,Resource resource) const {
  const uint8_t i=static_cast<uint8_t>(resource);
  if(!owner || i>=kResourceCount) return false;
  portENTER_CRITICAL(&mux_);
  const bool result=owners_[i]==owner;
  portEXIT_CRITICAL(&mux_);
  return result;
}
uint32_t ResourceManager::ownedMask(uint8_t owner) const {
  if(!owner) return 0;
  uint32_t mask=0;
  portENTER_CRITICAL(&mux_);
  for(uint8_t i=0;i<kResourceCount;i++) if(owners_[i]==owner) mask|=(1UL<<i);
  portEXIT_CRITICAL(&mux_);
  return mask;
}

bool AppManager::launch(AppModule& app) {
  if(state_!=AppState::Idle || app.id()==0) return false;
  state_=AppState::Starting;
  if(!resources_.acquireMask(app.id(),app.requiredResources())) {state_=AppState::Idle;return false;}
  app_=&app;
  if(!app_->begin()) {
    resources_.releaseOwner(app_->id());app_=nullptr;state_=AppState::Fault;return false;
  }
  state_=AppState::Running;
  return true;
}
void AppManager::service() {
  if(!app_) {if(state_==AppState::Fault)state_=AppState::Idle;return;}
  if(state_==AppState::Running) app_->update();
  else if(state_==AppState::Stopping && app_->stopped()) {
    resources_.releaseOwner(app_->id());app_=nullptr;state_=AppState::Idle;
  }
}
void AppManager::stop() {
  if(!app_ || state_!=AppState::Running) return;
  state_=AppState::Stopping;
  app_->requestStop();
}

bool ProcessRegistry::add(TaskHandle_t handle,uint8_t owner) {
  if(!handle || !owner) return false;
  portENTER_CRITICAL(&mux_);
  size_t slot=kManagedTaskLimit;
  for(size_t i=0;i<kManagedTaskLimit;i++) {
    if(entries_[i].handle==handle){portEXIT_CRITICAL(&mux_);return false;}
    if(!entries_[i].handle && slot==kManagedTaskLimit) slot=i;
  }
  if(slot<kManagedTaskLimit) {entries_[slot].handle=handle;entries_[slot].owner=owner;}
  portEXIT_CRITICAL(&mux_);
  return slot<kManagedTaskLimit;
}
void ProcessRegistry::remove(TaskHandle_t handle) {
  if(!handle) return;
  portENTER_CRITICAL(&mux_);
  for(size_t i=0;i<kManagedTaskLimit;i++) if(entries_[i].handle==handle) entries_[i]={};
  portEXIT_CRITICAL(&mux_);
}
size_t ProcessRegistry::snapshot(ProcessInfo* out,size_t capacity) const {
  if(!out || !capacity) return 0;
  Entry local[kManagedTaskLimit]{};
  portENTER_CRITICAL(&mux_);
  memcpy(local,entries_,sizeof(local));
  portEXIT_CRITICAL(&mux_);
  size_t count=0;
  for(size_t i=0;i<kManagedTaskLimit && count<capacity;i++) {
    TaskHandle_t h=local[i].handle;
    if(!h) continue;
    ProcessInfo& item=out[count++];
    const char* name=pcTaskGetName(h);
    if(name) strncpy(item.name,name,sizeof(item.name)-1);
    item.owner=local[i].owner;
    item.state=eTaskGetState(h);
    item.priority=uxTaskPriorityGet(h);
    item.stackHighWaterWords=uxTaskGetStackHighWaterMark(h);
  }
  return count;
}

} // namespace johnny
