// The platform's HTTP stack on macOS: NSURLSession, with the system's TLS, certificates and proxies. Compiled with ARC.

#include "Http.h"
#include "Sha256.h"

#import <Foundation/Foundation.h>
#include <objc/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace reckless::net
{
namespace
{
/** The session delegate's one method: every redirect refused, so the 3xx comes back as itself and Fetch follows it by
    the suite's rule - which drops the credentials as it leaves their origin - wherever the request came from.
    @param completion  told nil: do not follow */
void RefuseRedirect(id, SEL, NSURLSession *, NSURLSessionTask *, NSHTTPURLResponse *, NSURLRequest *,
                    void (^completion)(NSURLRequest *))
{
    completion(nil);
}

/** An address in this image alone, which names the delegate's class for it. */
int image_tag = 0;

/** The delegate's class, registered once an image under a name of the image's own - the address of a variable no other
    image shares - since the Objective-C runtime keeps one class a name a process, and two plug-ins carrying this code
    (a VST3 and an AU of one plugin in one host, two versions of a module) would otherwise meet over one name.
    @return  the class */
Class RedirectRefuserClass()
{
    static Class registered = [] {
        char name[128];
        std::snprintf(name, sizeof(name), "RecklessNetRedirectRefuser_%p", static_cast<const void *>(&image_tag));
        Class       cls      = objc_allocateClassPair([NSObject class], name, 0);
        const SEL   selector = @selector(URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:);
        const char *types = protocol_getMethodDescription(@protocol(NSURLSessionTaskDelegate), selector, NO, YES).types;
        class_addMethod(cls, selector, reinterpret_cast<IMP>(RefuseRedirect), types);
        class_addProtocol(cls, @protocol(NSURLSessionTaskDelegate));
        objc_registerClassPair(cls);
        return cls;
    }();
    return registered;
}

/** Text as an NSString.
    @param text  UTF-8
    @return      the string */
NSString *StringOf(const std::string &text)
{
    NSString *string = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return string != nil ? string : @"";
}

/** An NSString as UTF-8 text.
    @param string  the string, or nil
    @return        its text */
std::string TextOf(NSString *string)
{
    return string != nil ? std::string([string UTF8String]) : std::string();
}

/** NSURLSession behind the suite's transport: one ephemeral session - no cache, no cookie store, no credential store
    on disk - whose delegate refuses every redirect. */
class MacTransport final : public Transport
{
  public:
    /** The ephemeral session, its delegate refusing redirects. */
    MacTransport()
    {
        NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.HTTPShouldSetCookies       = NO;
        configuration.URLCache                   = nil;
        delegate_                                = [[RedirectRefuserClass() alloc] init];
        session_ = [NSURLSession sessionWithConfiguration:configuration delegate:delegate_ delegateQueue:nil];
    }

    /** The session invalidated, any task still in flight cancelled. */
    ~MacTransport() override
    {
        [session_ invalidateAndCancel];
    }

    Response Send(const Request &request, Cancellation &cancellation) override
    {
        Response response;
        NSURL   *url = [NSURL URLWithString:StringOf(request.url)];
        if (url == nil)
        {
            response.error = "not a URL: " + request.url;
            return response;
        }
        NSMutableURLRequest *outgoing = [NSMutableURLRequest requestWithURL:url];
        outgoing.HTTPMethod           = StringOf(request.method);
        outgoing.timeoutInterval      = request.timeout_seconds;
        outgoing.cachePolicy          = NSURLRequestReloadIgnoringLocalCacheData;
        for (const auto &header : request.headers)
        {
            [outgoing setValue:StringOf(header.value) forHTTPHeaderField:StringOf(header.name)];
        }
        if (!request.body.empty())
        {
            outgoing.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];
        }

        // The completion fills a block of its own, shared with this frame, so a completion that comes after a
        // cancelled wait has given up writes into memory that is still there.
        struct Outcome
        {
            Response             response;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
        };

        auto                  outcome = std::make_shared<Outcome>();
        NSURLSessionDataTask *task =
            [session_ dataTaskWithRequest:outgoing
                        completionHandler:^(NSData *data, NSURLResponse *answer, NSError *error) {
                          Response &r = outcome->response;
                          if (error != nil)
                          {
                              r.cancelled = error.code == NSURLErrorCancelled;
                              r.error     = r.cancelled ? std::string("cancelled") : TextOf(error.localizedDescription);
                          }
                          else if (auto *http = [answer isKindOfClass:[NSHTTPURLResponse class]]
                                                    ? static_cast<NSHTTPURLResponse *>(answer)
                                                    : nil)
                          {
                              r.status = static_cast<int>(http.statusCode);
                              [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *) {
                                if ([key isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]])
                                {
                                    r.headers.push_back({TextOf(key), TextOf(value)});
                                }
                              }];
                              if (data != nil)
                              {
                                  r.body.assign(static_cast<const char *>(data.bytes), data.length);
                              }
                          }
                          else
                          {
                              r.error = "no HTTP response";
                          }
                          dispatch_semaphore_signal(outcome->done);
                        }];
        {
            const Cancellation::Watch watch(cancellation, [task] { [task cancel]; });
            [task resume];
            // The task's own timeout ends it; the cancellation's hook ends it at once. Either way the completion runs.
            dispatch_semaphore_wait(outcome->done, DISPATCH_TIME_FOREVER);
        }
        if (cancellation.cancelled() && outcome->response.error.empty())
        {
            outcome->response           = Response();
            outcome->response.cancelled = true;
            outcome->response.error     = "cancelled";
        }
        return std::move(outcome->response);
    }

  private:
    id            delegate_{nil}; ///< the redirect refuser; the session holds it too, until invalidated
    NSURLSession *session_{nil};  ///< the session every request runs on
};
} // namespace

std::unique_ptr<Transport> MakePlatformTransport()
{
    return std::make_unique<MacTransport>();
}

void FillRandom(std::span<std::uint8_t> bytes)
{
    arc4random_buf(bytes.data(), bytes.size());
}
} // namespace reckless::net
