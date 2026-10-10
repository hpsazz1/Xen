#ifndef RECOIL_TUNER_PHASE_WINDOW_INTERNAL_H
#define RECOIL_TUNER_PHASE_WINDOW_INTERNAL_H
#include "recoil_tuner/recoil_tuner.h"
#include <algorithm>
#include <limits>

namespace recoil_tuner::detail {
struct PhaseVertexBounds {
    std::array<double,2> low{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()};
    std::array<double,2> high{-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
};
// 仅索引已验证的有限、严格升时序顶点；插值端点仍由原采样函数处理。
class PhaseVertexIndex {
public:
    explicit PhaseVertexIndex(const std::vector<CurvePoint>& curve):curve_(curve) {
        if(curve.size()<=64)return;
        leaves_=1;while(leaves_<curve.size())leaves_*=2;
        tree_.resize(leaves_*2);
        for(std::size_t i=0;i<curve.size();++i) {
            tree_[leaves_+i].low=tree_[leaves_+i].high={curve[i].x_counts,curve[i].y_counts};
        }
        for(std::size_t i=leaves_-1;i>0;--i)tree_[i]=merge(tree_[i*2],tree_[i*2+1]);
    }
    PhaseVertexBounds query(double first_ms,double last_ms) const {
        const auto first=std::lower_bound(curve_.begin(),curve_.end(),first_ms,
            [](const CurvePoint& point,double value){return point.time_ms<value;});
        if(!leaves_) {
            PhaseVertexBounds result;
            for(auto it=first;it!=curve_.end()&&it->time_ms<=last_ms;++it) {
                PhaseVertexBounds point;point.low=point.high={it->x_counts,it->y_counts};
                result=merge(result,point);
            }
            return result;
        }
        const auto last=std::upper_bound(first,curve_.end(),last_ms,
            [](double value,const CurvePoint& point){return value<point.time_ms;});
        auto left=static_cast<std::size_t>(first-curve_.begin())+leaves_;
        auto right=static_cast<std::size_t>(last-curve_.begin())+leaves_;
        PhaseVertexBounds before,after;
        // 左右按原顶点顺序合并；相等值保留先出现者，包括正负零。
        while(left<right) {
            if(left&1)before=merge(before,tree_[left++]);
            if(right&1)after=merge(tree_[--right],after);
            left/=2;right/=2;
        }
        return merge(before,after);
    }
private:
    static PhaseVertexBounds merge(const PhaseVertexBounds& first,const PhaseVertexBounds& last) {
        PhaseVertexBounds result;
        for(int axis=0;axis<2;++axis) {
            result.low[axis]=std::min(first.low[axis],last.low[axis]);
            result.high[axis]=std::max(first.high[axis],last.high[axis]);
        }
        return result;
    }
    const std::vector<CurvePoint>& curve_;
    std::size_t leaves_=0;
    std::vector<PhaseVertexBounds> tree_;
};
}
#endif
