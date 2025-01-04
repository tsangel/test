"use client"

import React, { useState } from "react";

const Counter = () => {
  const [count, setCount] = useState(0); // 초기 상태를 0으로 설정

  const handleClick = () => {
    setCount(count + 1); // count를 1씩 증가
  };

  return (
    <div style={{ display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", height: "100vh" }}>
      <h1 style={{ fontSize: "24px", fontWeight: "bold", marginBottom: "16px" }}>Count: {count}</h1>
      <button
        onClick={handleClick}
        style={{
          padding: "12px 24px",
          backgroundColor: "#007bff",
          color: "#fff",
          border: "none",
          borderRadius: "4px",
          cursor: "pointer",
          fontSize: "16px",
        }}
      >
        Increase Count
      </button>
    </div>
  );
};

  
export default function Page() {
  

    return <Counter />;
    // return <p>Invoice Page</p>;
}