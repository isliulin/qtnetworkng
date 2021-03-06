#include "../include/private/coroutine_p.h"
#include <windows.h>
#include <winbase.h>

QTNETWORKNG_NAMESPACE_BEGIN

class BaseCoroutinePrivate
{
public:
    BaseCoroutinePrivate(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize);
    virtual ~BaseCoroutinePrivate();
    bool initContext();
    bool raise(CoroutineException *exception = nullptr);
    bool yield();
private:
    BaseCoroutine * const q_ptr;
    BaseCoroutine * const previous;
    size_t stackSize;
    enum BaseCoroutine::State state;
    CoroutineException *exception;
    LPVOID context;
    bool bad;
    Q_DECLARE_PUBLIC(BaseCoroutine)
private:
    static void CALLBACK run_stub(BaseCoroutinePrivate *coroutine);
    void cleanup() { q_ptr->cleanup(); }
    friend BaseCoroutine* createMainCoroutine();
};

void CALLBACK BaseCoroutinePrivate::run_stub(BaseCoroutinePrivate *coroutine)
{
    coroutine->state = BaseCoroutine::Started;
    coroutine->q_ptr->started.callback(coroutine->q_ptr);
    try {
        coroutine->q_ptr->run();
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(const CoroutineExitException &) {
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(const CoroutineException &e) {
//        qDebug() << "got coroutine exception:" << e.what();
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
    } catch(...) {
        qWarning("coroutine throw a unhandled exception.");
        coroutine->state = BaseCoroutine::Stopped;
        coroutine->q_ptr->finished.callback(coroutine->q_ptr);
        //throw; // cause undefined behaviors
    }
    coroutine->cleanup();
}


BaseCoroutinePrivate::BaseCoroutinePrivate(BaseCoroutine *q, BaseCoroutine *previous, size_t stackSize)
    :q_ptr(q), previous(previous), stackSize(stackSize), state(BaseCoroutine::Initialized), exception(nullptr), context(nullptr),  bad(false)
{

}


BaseCoroutinePrivate::~BaseCoroutinePrivate()
{
    Q_Q(BaseCoroutine);
    if(currentCoroutine().get() == q) {
        qWarning("do not delete one self.");
    }
    if(context) {
        if(Q_UNLIKELY(stackSize == 0)) {
            ConvertFiberToThread();
        } else {
            DeleteFiber(context);
        }
    }
    if(exception)
        delete exception;
}


bool BaseCoroutinePrivate::initContext()
{
    if(context)
        return true;

    context = CreateFiberEx(1024 * 4, stackSize, 0, (PFIBER_START_ROUTINE)BaseCoroutinePrivate::run_stub, this);
    if(!context) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral("can not create fiber: error is %1").arg(error);
        bad = true;
        return false;
    } else {
        bad = false;
    }
    return true;
}


bool BaseCoroutinePrivate::raise(CoroutineException *exception)
{
    Q_Q(BaseCoroutine);
    if(currentCoroutine().get() == q) {
        qWarning("can not kill oneself.");
        return false;
    }

    if (this->exception) {
        qWarning("coroutine had been killed.");
        return false;
    }

    if (state == BaseCoroutine::Stopped || state == BaseCoroutine::Joined) {
        qWarning("coroutine is stopped.");
        return false;
    }

    if (exception) {
        this->exception = exception;
    } else {
        this->exception = new CoroutineExitException();
    }

    try {
        bool result = yield();
        delete exception;
        return result;
    } catch (...) {
        delete exception;
        throw;
    }
}

bool BaseCoroutinePrivate::yield()
{
    Q_Q(BaseCoroutine);

    if(bad || (state != BaseCoroutine::Initialized && state != BaseCoroutine::Started))
        return false;

    if(!initContext())
        return false;

    BaseCoroutine *old = currentCoroutine().get();
    if (!old || old == q)
        return false;

    currentCoroutine().set(q);
    SwitchToFiber(context);
    if (currentCoroutine().get() != old) { // when coroutine finished, swapcontext auto yield to the previous.
        currentCoroutine().set(old);
    }
    CoroutineException *e = old->d_func()->exception;
    if (e) {
        old->d_func()->exception = nullptr;
//        if(!dynamic_cast<CoroutineExitException*>(e)) {
//            qDebug() << "got exception:" << e->what() << old;
//        }
        e->raise();
    }
    return true;
}

BaseCoroutine* createMainCoroutine()
{
    BaseCoroutine *main = new BaseCoroutine(nullptr, 0);
    if (!main)
        return nullptr;
    BaseCoroutinePrivate *mainPrivate = main->d_func();
#if ( _WIN32_WINNT > 0x0600)
        if (IsThreadAFiber()) {
            mainPrivate->context = GetCurrentFiber();
        } else {
            mainPrivate->context = ConvertThreadToFiberEx(nullptr, 0);
        }
#else
        mainPrivate->context = ConvertThreadToFiberEx(nullptr, 0);
        if (Q_UNLIKELY(nullptr== mainPrivate->context)) {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_FIBER) {
                mainPrivate->context = GetCurrentFiber();
            }
            if (reinterpret_cast<LPVOID>(0x1E00) == mainPrivate->context) {
                mainPrivate->context = nullptr;
            }
        }
#endif
    if (!mainPrivate->context) {
        DWORD error = GetLastError();
        qWarning() << QStringLiteral("Coroutine can not malloc new memroy: error is %1").arg(error);
        delete main;
        return nullptr;
    }
    mainPrivate->state = BaseCoroutine::Started;
    return main;
}


// here comes the public class.
BaseCoroutine::BaseCoroutine(BaseCoroutine *previous, size_t stackSize)
    :dd_ptr(new BaseCoroutinePrivate(this, previous, stackSize))
{
}


BaseCoroutine::~BaseCoroutine()
{
    delete dd_ptr;
}


BaseCoroutine::State BaseCoroutine::state() const
{
    Q_D(const BaseCoroutine);
    return d->state;
}


bool BaseCoroutine::raise(CoroutineException *exception)
{
    Q_D(BaseCoroutine);
    return d->raise(exception);
}


bool BaseCoroutine::yield()
{
    Q_D(BaseCoroutine);
    return d->yield();
}


void BaseCoroutine::setState(BaseCoroutine::State state)
{
    Q_D(BaseCoroutine);
    d->state = state;
}


void BaseCoroutine::cleanup()
{
    Q_D(BaseCoroutine);
    if(d->previous) {
        d->previous->yield();
    }
}


BaseCoroutine *BaseCoroutine::previous() const
{
    Q_D(const BaseCoroutine);
    return d->previous;
}

QTNETWORKNG_NAMESPACE_END
