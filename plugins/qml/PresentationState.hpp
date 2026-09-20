#pragma once

#include <QColor>
#include <QRectF>
#include <cstdint>

class PresentationState {
public:
    struct Config {
        bool          valid { false };
        std::uint64_t bufferGeneration { 0 };
        std::uint64_t configGeneration { 0 };
        QRectF        sourceRect;
        QRectF        destRect;
        QColor        clearColor { Qt::black };
        std::uint32_t transform { 0 };
    };

    struct Content {
        bool          valid { false };
        std::uint64_t bufferGeneration { 0 };
        std::uint64_t contentToken { 0 };
        std::uint64_t presentationConfigGeneration { 0 };
        int           width { 0 };
        int           height { 0 };
        std::uint32_t fourcc { 0 };
        Config        config;
    };

    enum class ConfigResult
    {
        Rejected,
        Staged,
        DisplayedUpdated,
    };

    enum class PrepareResult
    {
        Rejected,
        AlreadyPrepared,
        Queued,
        Direct,
        Transition,
    };

    enum class AcceptResult
    {
        Rejected,
        Rebound,
        ContentChanged,
    };

    void beginIncoming(std::uint64_t bufferGeneration, std::uint64_t contentToken,
                       std::uint64_t presentationConfigGeneration, int width, int height,
                       std::uint32_t fourcc, bool valid) {
        m_incoming.valid                        = valid && contentToken != 0;
        m_incoming.bufferGeneration             = bufferGeneration;
        m_incoming.contentToken                 = contentToken;
        m_incoming.presentationConfigGeneration = presentationConfigGeneration;
        m_incoming.width                        = width;
        m_incoming.height                       = height;
        m_incoming.fourcc                       = fourcc;
        m_incoming.config                       = Config {};

        if (m_prepared.valid && ! m_prepared.promoted &&
            (m_prepared.content.bufferGeneration != bufferGeneration ||
             m_prepared.content.contentToken != contentToken ||
             m_prepared.content.presentationConfigGeneration != presentationConfigGeneration)) {
            m_prepared = Prepared {};
        }
    }

    void retireIncoming(std::uint64_t bufferGeneration) {
        if (m_incoming.bufferGeneration == bufferGeneration) m_incoming = Content {};
        if (m_prepared.valid && ! m_prepared.promoted &&
            m_prepared.content.bufferGeneration == bufferGeneration) {
            m_prepared = Prepared {};
        }
    }

    ConfigResult applyConfig(const Config& config) {
        if (m_displayed.valid && m_displayed.bufferGeneration == config.bufferGeneration) {
            m_displayed.config = config;
            if (m_incoming.valid && m_incoming.bufferGeneration == config.bufferGeneration) {
                m_incoming.config = config;
            }
            if (m_prepared.valid &&
                m_prepared.content.bufferGeneration == config.bufferGeneration) {
                m_prepared.content.config = config;
            }
            return ConfigResult::DisplayedUpdated;
        }
        if (! m_incoming.valid || m_incoming.bufferGeneration != config.bufferGeneration) {
            return ConfigResult::Rejected;
        }
        m_incoming.config = config;
        if (m_prepared.valid && m_prepared.content.bufferGeneration == config.bufferGeneration) {
            m_prepared.content.config = config;
        }
        return ConfigResult::Staged;
    }

    bool incomingFor(std::uint64_t bufferGeneration, Content& out) const {
        if (! m_incoming.valid || ! m_incoming.config.valid ||
            m_incoming.bufferGeneration != bufferGeneration) {
            return false;
        }
        out = m_incoming;
        return true;
    }

    PrepareResult prepare(const Content& content, bool transitionConfigured, bool outgoingAvailable,
                          std::uint64_t& serial) {
        if (! validIncoming(content)) return PrepareResult::Rejected;
        if (m_transitionActive) return PrepareResult::Queued;
        if (m_prepared.valid && sameIdentity(m_prepared.content, content)) {
            serial = m_prepared.serial;
            return PrepareResult::AlreadyPrepared;
        }

        m_prepared.valid      = true;
        m_prepared.serial     = ++m_serial;
        m_prepared.content    = content;
        m_prepared.transition = transitionConfigured && outgoingAvailable &&
                                m_committedToken != 0 && m_committedToken != content.contentToken;
        serial                = m_prepared.serial;
        return m_prepared.transition ? PrepareResult::Transition : PrepareResult::Direct;
    }

    bool promote(std::uint64_t serial) {
        if (! m_prepared.valid || m_prepared.serial != serial ||
            ! validIncoming(m_prepared.content)) {
            return false;
        }
        m_displayed         = m_prepared.content;
        m_prepared.promoted = true;
        return true;
    }

    AcceptResult accept(std::uint64_t serial, Content& accepted) {
        if (! m_prepared.valid || m_prepared.serial != serial || ! m_displayed.valid ||
            ! sameIdentity(m_displayed, m_prepared.content)) {
            return AcceptResult::Rejected;
        }
        accepted           = m_prepared.content;
        const bool changed = m_committedToken != accepted.contentToken;
        m_committedToken   = accepted.contentToken;
        m_prepared         = Prepared {};
        return changed ? AcceptResult::ContentChanged : AcceptResult::Rebound;
    }

    bool discard(std::uint64_t serial) {
        if (! m_prepared.valid || m_prepared.serial != serial) return false;
        m_prepared = Prepared {};
        return true;
    }

    bool bufferChangesWith(std::uint64_t bufferGeneration) const {
        return ! m_displayed.valid || m_displayed.bufferGeneration != bufferGeneration;
    }

    bool prepared(std::uint64_t serial, Content& content, bool& transition) const {
        if (! m_prepared.valid || m_prepared.serial != serial) return false;
        content    = m_prepared.content;
        transition = m_prepared.transition;
        return true;
    }

    Content       displayed() const { return m_displayed; }
    std::uint64_t committedToken() const { return m_committedToken; }
    bool          transitionActive() const { return m_transitionActive; }
    void          setTransitionActive(bool active) { m_transitionActive = active; }

    void reset() {
        m_incoming         = Content {};
        m_displayed        = Content {};
        m_prepared         = Prepared {};
        m_committedToken   = 0;
        m_serial           = 0;
        m_transitionActive = false;
    }

private:
    struct Prepared {
        bool          valid { false };
        std::uint64_t serial { 0 };
        Content       content;
        bool          transition { false };
        bool          promoted { false };
    };

    bool validIncoming(const Content& content) const {
        return content.valid && content.config.valid && content.contentToken != 0 &&
               content.bufferGeneration == content.config.bufferGeneration && m_incoming.valid &&
               m_incoming.config.valid && sameIdentity(content, m_incoming) &&
               content.config.configGeneration == m_incoming.config.configGeneration;
    }

    static bool sameIdentity(const Content& left, const Content& right) {
        return left.bufferGeneration == right.bufferGeneration &&
               left.contentToken == right.contentToken &&
               left.presentationConfigGeneration == right.presentationConfigGeneration;
    }

    Content       m_incoming;
    Content       m_displayed;
    Prepared      m_prepared;
    std::uint64_t m_committedToken { 0 };
    std::uint64_t m_serial { 0 };
    bool          m_transitionActive { false };
};
