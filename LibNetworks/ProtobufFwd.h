#pragma once

// protobuf 전방 선언 — 모듈 인터페이스(.ixx) GMF 에서 <google/protobuf/message.h> 대신 include.
//
// 인터페이스 GMF 에서 protobuf 헤더를 include 하면 IFC 에 protobuf 정의가 실려, .pb.h 를 include 한
// importer 에서 MSVC 14.51.36231 이 C1116 ICE (google::protobuf::internal::MessageCreator::New<MessageLite>)
// 를 낸다. 완전한 정의가 필요한 곳은 구현 .cpp 의 GMF 에서 include.
//
// GMF 에는 전처리기 지시문만 올 수 있으므로 (C5202) 선언을 직접 쓰지 않고 이 헤더로 분리.
namespace google::protobuf
{
class Message;
}
