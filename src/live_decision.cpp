#include "live_decision.h"

LiveDecision::State LiveDecision::observe(const QString& matchedName, bool facePresent,
                                          std::uint64_t frameNumber, Clock::time_point capturedAt) {
    const bool consecutive = frameNumber == previousFrame_ + 1;
    previousFrame_ = frameNumber;

    // 开锁复位只看画面是否有人脸。匹配分数低、误识别和多人入镜都保留本次开锁记录。
    if (facePresent) {
        absentSince_.reset();
    } else if (unlockedDuringPresence_) {
        // 跳过的采集帧可能有人脸，不能计入连续无人脸的时间。
        if (!absentSince_ || !consecutive) {
            absentSince_ = capturedAt;
        }
        if (capturedAt - *absentSince_ >= rearmDelay) {
            unlockedDuringPresence_ = false;
            absentSince_.reset();
        }
    }

    if (!authorizedName_.isEmpty()) {
        if (matchedName == authorizedName_) {
            deniedSince_.reset();
            return {authorizedName_};
        }
        // 短暂丢脸、低分或认成另一人时，维持上次授权。原身份重新通过就清零计时。
        if (!deniedSince_) {
            deniedSince_ = capturedAt;
        }
        if (capturedAt - *deniedSince_ < rearmDelay) {
            return {authorizedName_};
        }
        authorizedName_.clear();
        deniedSince_.reset();
    }

    if (matchedName.isEmpty()) {
        candidateName_.clear();
        matchingFrames_ = 0;
        return {};
    }

    // 换人或采集序号跳过时重新计数。被最新帧覆盖的图像没有验证结果。
    if (!consecutive || matchedName != candidateName_) {
        candidateName_ = matchedName;
        matchingFrames_ = 0;
    }
    if (++matchingFrames_ < requiredMatches) {
        return {{}, matchingFrames_};
    }

    authorizedName_ = candidateName_;
    candidateName_.clear();
    matchingFrames_ = 0;
    const bool shouldUnlock = !unlockedDuringPresence_;
    unlockedDuringPresence_ = true;
    return {authorizedName_, requiredMatches, shouldUnlock};
}

void LiveDecision::forgetTemplate(const QString& name) {
    if (candidateName_ == name) {
        candidateName_.clear();
        matchingFrames_ = 0;
    }
    if (authorizedName_ == name) {
        authorizedName_.clear();
        deniedSince_.reset();
    }
    // 删除模板不会让画面变成无人脸，保留本次入镜的开锁记录。
}
