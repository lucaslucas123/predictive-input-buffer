#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <algorithm>
#include <array>
#include <deque>
using namespace geode::prelude;

// Experimental: forecast current solid/hazard geometry, not future triggers.
namespace pib {
    constexpr std::array<int,5> windows{2,4,8,16,0};
    int mode=4, lastDelay=0;
    unsigned generation=0;
    bool predicting=false, dead=false;
    PlayerObject* probe=nullptr;
    PlayLayer* owner=nullptr;
    float step=0.25f;
    bool isProbe(PlayerObject* p) { return predicting && p==probe; }
    std::string status() {
        if(mode==4) return "PIB experimental: OFF | F6";
        return fmt::format("PIB: mode {} | {} ticks | wave: RAW | last +{} | F6",mode+1,
            windows[mode],lastDelay);
    }
}
struct Pending { bool down, player1; int ticks, delay; };
class $modify(PIBGame, GJBaseGameLayer) {
    struct Fields {
        std::deque<Pending> pending;
        std::array<bool,2> applied{false,false};
        unsigned generation=0;
    };
    int survival(PlayerObject* real,bool oldDown,bool down,int delay,int horizon) {
        auto p=pib::probe;
        p->copyAttributes(real);
        p->m_gravityMod=real->m_gravityMod;
        p->m_isOnGround=real->m_isOnGround;
        p->m_isDead=false;
        p->setVisible(false);
        pib::dead=false;
        oldDown?p->pushButton(PlayerButton::Jump):p->releaseButton(PlayerButton::Jump);
        for(int tick=0;tick<horizon;++tick) {
            if(tick==delay) down?p->pushButton(PlayerButton::Jump):p->releaseButton(PlayerButton::Jump);
            p->m_collisionLogTop->removeAllObjects();
            p->m_collisionLogBottom->removeAllObjects();
            p->m_collisionLogLeft->removeAllObjects();
            p->m_collisionLogRight->removeAllObjects();
            checkCollisions(p,pib::step,false);
            if(pib::dead) return tick;
            p->update(pib::step);
            if(pib::dead || p->m_isDead) return tick;
        }
        return horizon;
    }
    int chooseDelay(PlayerObject* real,bool down,bool oldDown,int limit) {
        if(!pib::probe || static_cast<GJBaseGameLayer*>(pib::owner)!=static_cast<GJBaseGameLayer*>(this) || !real || real->m_isDead || real->m_isPlatformer || real->m_isDart) return 0;
        int horizon=limit+24;

        pib::predicting=true;
        int best=survival(real,oldDown,down,0,horizon), selected=0;
        if(best<horizon) for(int delay=1;delay<=limit;++delay) {
            int score=survival(real,oldDown,down,delay,horizon);
            if(score==horizon) { selected=delay; break; }

        }

        pib::predicting=false;
        return selected;
    }
    void flushPending() {
        auto f=m_fields.self();
        for(auto const& e:f->pending) {
            GJBaseGameLayer::handleButton(e.down,1,e.player1);
            f->applied[e.player1?0:1]=e.down;
        }
        f->pending.clear(); f->generation=pib::generation;
    }
    void handleButton(bool down,int button,bool player1) {
        if(pib::predicting) return;
        auto f=m_fields.self();
        if(f->generation!=pib::generation) flushPending();
        auto real=player1?m_player1:m_player2;
        if(button==1 && real && real->m_isDart) {
            // Never run a cloned wave through PlayerObject::update. Preserve
            // already queued events before forwarding the new physical input.
            for(auto it=f->pending.begin();it!=f->pending.end();) {
                if(it->player1==player1) {
                    GJBaseGameLayer::handleButton(it->down,1,player1);
                    f->applied[player1?0:1]=it->down;
                    it=f->pending.erase(it);
                } else ++it;
            }
            GJBaseGameLayer::handleButton(down,button,player1);
            f->applied[player1?0:1]=down;
            pib::lastDelay=0;
            return;
        }
        if(button!=1 || pib::mode==4 || static_cast<GJBaseGameLayer*>(pib::owner)!=static_cast<GJBaseGameLayer*>(this)) {
            GJBaseGameLayer::handleButton(down,button,player1);
            if(button==1) f->applied[player1?0:1]=down;
            return;
        }
        // Preserve input order: a release must not overtake its delayed press.
        int preceding=0;
        for(auto const& e:f->pending) if(e.player1==player1) preceding=std::max(preceding,e.delay);
        int limit=pib::windows[pib::mode];
        if(real && real->m_isDart) limit=std::min(limit,8);
        int delay=preceding?preceding:chooseDelay(real,down,f->applied[player1?0:1],limit);
        pib::lastDelay=delay;
        if(delay>0) f->pending.push_back({down,player1,delay,delay});
        else { GJBaseGameLayer::handleButton(down,button,player1); f->applied[player1?0:1]=down; }
    }
    void processCommands(float dt,bool half,bool last) {
        auto f=m_fields.self();
        if(f->generation!=pib::generation) flushPending();
        for(auto it=f->pending.begin();it!=f->pending.end();) {
            auto player=it->player1?m_player1:m_player2;
            if((player && player->m_isDart) || --it->ticks<=0) {
                GJBaseGameLayer::handleButton(it->down,1,it->player1);
                f->applied[it->player1?0:1]=it->down;
                it=f->pending.erase(it);
            } else ++it;
        }
        GJBaseGameLayer::processCommands(dt,half,last);
        if(static_cast<GJBaseGameLayer*>(pib::owner)==static_cast<GJBaseGameLayer*>(this)) if(auto label=typeinfo_cast<CCLabelBMFont*>(getChildByID("pib-status")))
            label->setString(pib::status().c_str());
    }
    void collisionCheckObjects(PlayerObject* p,gd::vector<GameObject*>* objects,int count,float dt) {
        if(!pib::isProbe(p)) { GJBaseGameLayer::collisionCheckObjects(p,objects,count,dt); return; }
        gd::vector<GameObject*> geometry;
        for(int i=0;i<count;++i) {
            auto obj=objects->at(i); auto t=obj->m_objectType;
            if(t==GameObjectType::Solid || t==GameObjectType::Hazard ||
               t==GameObjectType::AnimatedHazard || t==GameObjectType::Slope) geometry.push_back(obj);
        }
        GJBaseGameLayer::collisionCheckObjects(p,&geometry,static_cast<int>(geometry.size()),dt);
    }
    bool canBeActivatedByPlayer(PlayerObject* p,EffectGameObject* obj) {
        return !pib::isProbe(p) && GJBaseGameLayer::canBeActivatedByPlayer(p,obj);
    }
    void playerTouchedRing(PlayerObject* p,RingObject* ring) {
        if(!pib::isProbe(p)) GJBaseGameLayer::playerTouchedRing(p,ring);
    }
    void flipGravity(PlayerObject* p,bool a,bool b) {
        if(!pib::isProbe(p)) GJBaseGameLayer::flipGravity(p,a,b);
    }
};
class $modify(PIBPlay, PlayLayer) {
    bool init(GJGameLevel* level,bool replay,bool noObjects) {
        if(!PlayLayer::init(level,replay,noObjects)) return false;
        pib::owner=this;
        pib::probe=PlayerObject::create(1,1,this,this,true);
        if(pib::probe) { pib::probe->setVisible(false); m_objectLayer->addChild(pib::probe); }
        auto label=CCLabelBMFont::create(pib::status().c_str(),"bigFont.fnt");
        label->setID("pib-status"); label->setScale(0.25f); label->setAnchorPoint({0,1});
        label->setPosition({8,CCDirector::sharedDirector()->getWinSize().height-8});
        addChild(label,1000);
        return true;
    }
    void resetLevel() override {
        auto base=static_cast<PIBGame*>(static_cast<GJBaseGameLayer*>(this));
        base->m_fields->pending.clear(); base->m_fields->applied={false,false};
        pib::lastDelay=0;
        PlayLayer::resetLevel();
    }
    void destroyPlayer(PlayerObject* p,GameObject* obj) override {
        if(pib::isProbe(p)) { pib::dead=true; return; }
        PlayLayer::destroyPlayer(p,obj);
    }
    void playEndAnimationToPos(CCPoint pos) { if(!pib::predicting) PlayLayer::playEndAnimationToPos(pos); }
    void onQuit() {
        if(static_cast<GJBaseGameLayer*>(pib::owner)==static_cast<GJBaseGameLayer*>(this)) { pib::probe=nullptr; pib::owner=nullptr; }
        PlayLayer::onQuit();
    }
};
class $modify(PIBPlayer, PlayerObject) {
    void update(float dt) override {
        if(!pib::predicting && pib::owner && (this==pib::owner->m_player1 || this==pib::owner->m_player2)) pib::step=dt;
        PlayerObject::update(dt);
    }
    void incrementJumps() { if(!pib::predicting) PlayerObject::incrementJumps(); }
    void ringJump(RingObject* ring,bool a) { if(!pib::isProbe(this)) PlayerObject::ringJump(ring,a); }
    void playSpiderDashEffect(CCPoint from,CCPoint to) { if(!pib::isProbe(this)) PlayerObject::playSpiderDashEffect(from,to); }
};
class $modify(PIBStreak, HardStreak) {
    void addPoint(CCPoint pos) { if(!pib::predicting) HardStreak::addPoint(pos); }
};
class $modify(PIBEffect, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer,int a,gd::vector<int> const* b) override {
        if(!pib::predicting) EffectGameObject::triggerObject(layer,a,b);
    }
};
class $modify(PIBKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key,bool down,bool repeat,double timestamp) {
        if(key==KEY_F6) {
            if(down && !repeat) { pib::mode=(pib::mode+1)%5; ++pib::generation; pib::lastDelay=0; log::info("{}",pib::status()); }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key,down,repeat,timestamp);
    }
};
