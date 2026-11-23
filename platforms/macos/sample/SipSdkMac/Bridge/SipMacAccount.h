#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SipMacRegistrationState) {
    SipMacRegistrationStateUnregistered = 0,
    SipMacRegistrationStateRegistering,
    SipMacRegistrationStateRegistered,
    SipMacRegistrationStateFailed
};

@class SipMacCall;
@class SipMacClient;
struct sip_account_t;

@interface SipMacAccount : NSObject

@property (nonatomic, readonly) uint64_t accountId;
@property (nonatomic, readonly, copy) NSString *aor;
@property (nonatomic, readonly, copy) NSString *user;
@property (nonatomic, readonly, copy) NSString *domain;
@property (nonatomic, readonly) BOOL isRegistered;
@property (nonatomic, readonly) SipMacRegistrationState regState;

- (instancetype)initWithHandle:(struct sip_account_t *)handle
                        client:(nullable SipMacClient *)client
                    ownsHandle:(BOOL)ownsHandle;

- (int)reregister;
- (int)unregister;
- (nullable SipMacCall *)dial:(NSString *)uri withVideo:(BOOL)withVideo;

// Messaging (SIP MESSAGE). Return the local message id, or 0 when the message was not queued.
- (uint64_t)sendMessageToUri:(NSString *)uri text:(NSString *)text;
- (uint64_t)sendMessageToUri:(NSString *)uri body:(NSData *)body contentType:(NSString *)contentType;

- (void)invalidate;
- (nullable struct sip_account_t *)handle;

@end

NS_ASSUME_NONNULL_END
